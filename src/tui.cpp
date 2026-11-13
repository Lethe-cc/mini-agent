// Self-drawn minimal TUI (ADR 003) implementing the frame model and
// interaction contract of design 007 section 5.6.
//
// Rendering (5.6 rule 2; supersedes 5.4's "alternate screen" note): inline
// main-screen rendering. Committed history prints once and lives in the
// terminal's native scrollback; only a small "live region" (response tail,
// composer, status) is redrawn in place. No alternate screen, no full diff.
#include "mini_agent/tui.hpp"

#include "mini_agent/highlight.hpp"
#include "mini_agent/tui_bridge.hpp"

#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/models_dev.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mini_agent::tui {
namespace {

constexpr std::string_view kCommands[] = {
    "/approve", "/config",   "/context", "/cwd",     "/debug",   "/exit",     "/help",
    "/login",   "/mcp",      "/model",   "/models",  "/preview", "/provider", "/reasoning",
    "/session", "/sessions", "/sidebar", "/sysinfo", "/tools"};

class RawMode {
  public:
    explicit RawMode(int fd) {
        if (::tcgetattr(fd, &saved_) == 0) {
            struct termios raw = saved_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
            // ISIG off: ^C/^D arrive as 0x03/0x04 bytes for our keymap
            // (design 007 section 5.6); SIGINT is ignored at entry anyway.
            raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL); // we see \r ourselves
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            active_ = ::tcsetattr(fd, TCSANOW, &raw) == 0;
        }
    }
    ~RawMode() {
        if (active_)
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

  private:
    struct termios saved_{};
    bool active_ = false;
};

int term_columns() {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

int term_rows() {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return ws.ws_row;
    }
    return 24;
}

// ---- delta buffer / tool events / approvals: worker<->UI handoff now
// lives in tui::TurnBridge (tui_bridge.hpp, v4.17) so agent_test can
// exercise the thread boundary without a pty (008 D4). ----

struct TurnResult {
    bool ok = false;
    bool aborted = false;
    std::string reply;
    std::string error;
    // v4: provider usage rolled up by run_turn (zero when the gateway
    // reported nothing). Read by the main thread after join().
    TokenUsage usage;
    // v4.2: reasoning evidence observed in the responses (design 010 §11).
    ActualReasoning reasoning;
    // Credential evidence from the completion request. The worker fills these
    // in and the main thread folds them into `auth` (the worker must never
    // touch main-thread state such as `auth`, `logger` or `screen`).
    int http_status = 0;
    bool network_error = false;
};

} // namespace

// ---------------------------------------------------------------- pure helpers

std::vector<std::string_view> slash_commands() noexcept {
    return std::vector<std::string_view>(std::begin(kCommands), std::end(kCommands));
}

std::vector<std::string_view> slash_matches(std::string_view prefix) {
    std::vector<std::string_view> out;
    if (prefix.empty() || prefix.front() != '/')
        return out;
    for (const auto& cmd : kCommands) {
        if (cmd.size() >= prefix.size() && cmd.substr(0, prefix.size()) == prefix) {
            out.push_back(cmd);
        }
    }
    return out;
}

// v4.10: second-tier completion -- the argument choices a command accepts.
// Shown under the composer once the command is fully typed (or selected via
// Tab), and cycled by Tab exactly like the command list. Placeholders in
// angle brackets are hints, not values: Tab inserts them so the user fills
// the real value (ids/paths/keys are runtime data we cannot enumerate).
// v4.13 exception: /session CAN enumerate -- `session_ids` (the caller's
// SessionStore::list snapshot) replaces the `<id>` placeholder with real
// ids, filtered against the typed prefix by the usual prefix rule below.
// Empty `session_ids` keeps the placeholder behaviour (no store, no
// sessions yet).
std::vector<std::string> slash_arg_choices(std::string_view command,
                                           const std::vector<std::string>& session_ids = {},
                                           const std::vector<std::string>& provider_names = {},
                                           const std::vector<std::string>& model_choices = {}) {
    if (command == "/context")
        return {"<tokens>", "probe", "off"};
    if (command == "/log")
        return {"info", "debug", "trace", "<path>", "off", "default"};
    if (command == "/debug")
        return {"on", "all", "off"};
    if (command == "/session") {
        if (session_ids.empty())
            return {"<id>", "new"};
        std::vector<std::string> out = session_ids;
        out.push_back("new");
        return out;
    }
    if (command == "/approve")
        return {"ask", "allowlist", "off"};
    if (command == "/reasoning")
        return {"low", "medium", "high", "off"};
    if (command == "/provider")
        return {"list", "add", "rm", "<url>"};
    if (command == "/model") {
        // v6: fetched entries cycle before the free-text placeholder; in an
        // aggregated fetch they carry the qualified provider/model syntax.
        std::vector<std::string> out = model_choices;
        out.push_back("<name>");
        return out;
    }
    if (command == "/login") {
        std::vector<std::string> out = provider_names; // /login <name> [key]
        out.push_back("<key>");
        return out;
    }
    if (command == "/cwd")
        return {"<path>"};
    if (command == "/preview")
        return {"<path>"};
    return {};
}

// Split a composer "/cmd rest" into (cmd, rest). rest is "" when the line
// is exactly "/cmd" or "/cmd " (the space variant keeps the cursor state
// meaningful for arg cycling).
static bool split_composer(std::string_view line, std::string_view* cmd, std::string_view* rest) {
    if (line.empty() || line.front() != '/')
        return false;
    const auto space = line.find(' ');
    if (space == std::string_view::npos) {
        *cmd = line;
        *rest = std::string_view{};
        return true;
    }
    *cmd = line.substr(0, space);
    *rest = line.substr(space + 1);
    // Single-word argument state participates in completion (empty rest
    // covers "/cmd "); a second space means the user is typing a real
    // value (path, url) -- the line is final, no cycling.
    return rest->find(' ') == std::string_view::npos;
}

std::size_t utf8_append(std::string& line, std::string_view bytes) {
    line.append(bytes);
    return bytes.size();
}

bool utf8_backspace(std::string& line) noexcept {
    if (line.empty())
        return false;
    std::size_t i = line.size();
    do {
        --i;
    } while (i > 0 && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80);
    line.erase(i);
    return true;
}

std::size_t utf8_codepoints(std::string_view text) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        i += std::min(len, text.size() - i);
        ++n;
    }
    return n;
}

namespace {
bool utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}
} // namespace

std::size_t utf8_next(std::string_view text, std::size_t pos) noexcept {
    if (pos >= text.size())
        return text.size();
    ++pos;
    while (pos < text.size() && utf8_continuation(static_cast<unsigned char>(text[pos])))
        ++pos;
    return pos;
}

std::size_t utf8_prev(std::string_view text, std::size_t pos) noexcept {
    if (pos == 0)
        return 0;
    --pos;
    while (pos > 0 && utf8_continuation(static_cast<unsigned char>(text[pos])))
        --pos;
    return pos;
}

void utf8_insert(std::string& line, std::size_t& pos, std::string_view bytes) {
    line.insert(pos, bytes);
    pos += bytes.size();
}

bool utf8_erase_before(std::string& line, std::size_t& pos) noexcept {
    if (pos == 0)
        return false;
    const std::size_t start = utf8_prev(line, pos);
    line.erase(start, pos - start);
    pos = start;
    return true;
}

bool utf8_erase_at(std::string& line, std::size_t& pos) noexcept {
    if (pos >= line.size())
        return false;
    line.erase(pos, utf8_next(line, pos) - pos);
    return true;
}

std::optional<std::string> model_choice(const std::vector<std::string>& models,
                                        std::string_view input) {
    if (models.empty() || input.empty())
        return std::nullopt;
    // Exact id match wins: ids are opaque and may legitimately be numeric.
    for (const auto& id : models)
        if (id == input)
            return id;
    for (const char ch : input)
        if (ch < '0' || ch > '9')
            return std::nullopt;
    std::size_t index = 0;
    for (const char ch : input) {
        index = index * 10 + static_cast<std::size_t>(ch - '0');
        if (index > models.size())
            return std::nullopt; // reject early: also guards overflow
    }
    if (index == 0)
        return std::nullopt;
    return models[index - 1];
}

std::vector<std::string> model_menu_rows(const std::vector<std::string>& models,
                                         std::string_view current) {
    std::vector<std::string> rows;
    if (models.empty())
        return rows;
    rows.push_back("models (" + std::to_string(models.size()) +
                   "): type a number, or /model <name>");
    for (std::size_t i = 0; i < models.size(); ++i) {
        std::string row = "  " + std::to_string(i + 1) + ". " + models[i];
        if (!current.empty() && models[i] == current)
            row += "  <- current";
        rows.push_back(std::move(row));
    }
    rows.push_back("Enter picks; Esc dismisses; /model <name> still works");
    return rows;
}

namespace {

bool color_env_disabled() noexcept {
    const char* no_color = std::getenv("NO_COLOR"); // https://no-color.org
    if (no_color != nullptr && *no_color != '\0')
        return true;
    const char* term = std::getenv("TERM");
    return term != nullptr && std::strcmp(term, "dumb") == 0;
}

bool starts_with(std::string_view line, std::string_view prefix) noexcept {
    return line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0;
}

std::string tint(bool colors, std::string_view sgr, std::string_view text) {
    if (!colors)
        return std::string(text);
    std::string out;
    out.reserve(text.size() + sgr.size() + 6);
    out += "\x1b[";
    out += sgr;
    out += 'm';
    out += text;
    out += "\x1b[0m";
    return out;
}

} // namespace

bool colors_enabled() {
    // Cached process-lifetime constant: the TUI only starts behind cli_main's
    // isatty gate, and NO_COLOR/TERM never change mid-session. Static-local
    // init is thread-safe (C++11); first call happens on the main thread.
    static const bool on = ::isatty(STDOUT_FILENO) != 0 && !color_env_disabled();
    return on;
}

std::string style_history_line(std::string_view line, bool colors) {
    // Whole-line spans only: every printable substring stays contiguous for
    // copy/paste and test assertions, and md::wrap re-asserts the span on
    // wrapped continuation rows. 1 = bold, 2 = dim, 3x = colour, 1;3x = strong.
    if (!colors || line.find('\x1b') != std::string_view::npos)
        return std::string(line); // producer-styled (markdown replies) or off
    if (starts_with(line, "[auth]")) {
        if (line.find("login verified") != std::string_view::npos)
            return tint(colors, "32", line); // green: key accepted
        if (line.find("REJECTED") != std::string_view::npos)
            return tint(colors, "1;31", line); // bold red: provider refused
        return tint(colors, "33", line);       // yellow: unconfirmed/unreachable
    }
    if (starts_with(line, "[tool]")) {
        if (line.find("[denied]") != std::string_view::npos)
            return tint(colors, "1;31", line);
        if (line.find("[error ") != std::string_view::npos)
            return tint(colors, "33", line);
        return tint(colors, "36", line); // cyan: ran fine
    }
    if (starts_with(line, "[ok]") || starts_with(line, "saved session "))
        return tint(colors, "32", line);
    if (starts_with(line, "[error]"))
        return tint(colors, "1;31", line);
    if (starts_with(line, "[warn]") || starts_with(line, "[note]") ||
        starts_with(line, "[interrupted]"))
        return tint(colors, "33", line);
    if (starts_with(line, "[info]") || starts_with(line, "[input]"))
        return tint(colors, "36", line);
    if (starts_with(line, "[cancel]"))
        return tint(colors, "2", line);
    if (starts_with(line, "[hint]"))
        return tint(colors, "36", line);
    if (starts_with(line, "mini-agent · ") || starts_with(line, "You: "))
        return tint(colors, "1", line); // bold: identity rows
    if (starts_with(line, "config "))
        return tint(colors, line.find("· ready") != std::string_view::npos ? "32" : "2", line);
    // Everything below is informational echo / legend text and should recede.
    // Order matters: the 4-space AI-reply continuation check must not swallow
    // the longer-indented help legend rows, so the dim list runs first.
    if (starts_with(line, "approval="))
        return tint(colors, line.find("off") != std::string_view::npos ? "33" : "2", line);
    static constexpr std::string_view kDimPrefixes[] = {
        "commands:",    "prompt:", "Each of ",   "configure ", "Enter ",       "Tab comple",
        "restart serv", "tools (", "mcp server", "sessions:",  "resume after", "logging",
        "base_url=",    "model=",  "api_key=",   "login=",     "log_file=",    "config file:",
        "          ", // /help continuation (10 spaces)
        "        ",   // legend continuation (8 spaces)
    };
    for (const auto prefix : kDimPrefixes) {
        if (starts_with(line, prefix))
            return tint(colors, "2", line);
    }
    if (starts_with(line, "    "))
        return std::string(line); // assistant-reply continuation: leave plain
    if (starts_with(line, "  ") || starts_with(line, "ALLOW "))
        return tint(colors, "2", line); // list entries / approval prompt
    return std::string(line);
}

std::string config_next_hint(const AppConfig& config) {
    const int set = (config.base_url.empty() ? 0 : 1) + (config.api_key.empty() ? 0 : 1) +
                    (config.model.empty() ? 0 : 1);
    // Only the next step, in provider -> key -> model order: each command is
    // independently saved, so any of them can be supplied in its own sitting
    // (or from a different process) and this line reflects what is left.
    std::string next;
    if (config.base_url.empty())
        next = "/provider <url>";
    else if (config.api_key.empty())
        next = "/login <key>";
    else if (config.model.empty())
        next = "/models (list & pick) or /model <name>";
    const std::string progress = "config " + std::to_string(set) + "/3";
    if (next.empty())
        return progress + " · ready";
    return progress + " · next: " + next;
}

std::string auth_status_line(const AuthProbe& probe) {
    // Plain text on purpose: `LiveScreen::commit` adds the verdict colour via
    // `style_history_line`, so the pure helper stays testable without a TTY.
    const auto accepted = [](int status) { return status >= 200 && status < 300; };
    if (!probe.attempted)
        return "[auth] login unverified · /provider + /login, then /models to check";
    if (probe.fetch_ok)
        return "[auth] login verified · key accepted · " + std::to_string(probe.model_count) +
               " model(s) reachable";
    if (probe.http_status == 401 || probe.http_status == 403)
        return "[auth] login REJECTED · HTTP " + std::to_string(probe.http_status) +
               " · provider refused this key · re-enter /login <key>";
    if (accepted(probe.http_status))
        return "[auth] login verified · key accepted (model list unavailable, see above)";
    if (probe.http_status != 0)
        return "[auth] login unverified · provider replied HTTP " +
               std::to_string(probe.http_status) + " · key not confirmed";
    return "[auth] login unverified · provider unreachable · check /provider <url>";
}

std::string_view auth_log_token(const AuthProbe& probe) {
    const auto accepted = [](int status) { return status >= 200 && status < 300; };
    if (!probe.attempted)
        return "untried";
    if (probe.fetch_ok || accepted(probe.http_status))
        return "verified";
    if (probe.http_status == 401 || probe.http_status == 403)
        return "rejected";
    if (probe.http_status != 0)
        return "refused";
    return "unreachable";
}

ConnState connection_state(const AppConfig& config, const AuthProbe& probe, bool listing) noexcept {
    // Pure derivation: no request is ever made here. The verdict comes from the
    // model-list probe that /provider and /login already fire (rule 9), so the
    // prompt costs nothing and never probes at startup (cold-start budget).
    if (config.base_url.empty() || config.api_key.empty())
        return ConnState::Unconfigured;
    if (listing)
        return ConnState::Checking;
    const auto token = auth_log_token(probe);
    if (token == "verified")
        return ConnState::Verified;
    if (token == "rejected")
        return ConnState::Rejected;
    if (token == "unreachable")
        return ConnState::Unreachable;
    // `untried` and `refused` (a non-auth status code) both leave the key
    // unconfirmed: the prompt must not read as healthy without evidence.
    return ConnState::Unverified;
}

std::string prompt_marker(const ConnState state) noexcept {
    // Glyph and colour both carry the state (a colour-only signal is lost on
    // monochrome terminals and for colour-blind users). SGR is zero-width for
    // md::wrap, and each marker ends in a reset so no style can leak into the
    // composer text or the following rows (5.6 rule 2 row accounting).
    switch (state) {
    case ConnState::Unconfigured:
        return "\x1b[2m?\x1b[0m"; // dim: nothing to connect to yet
    case ConnState::Checking:
        return "\x1b[36m~\x1b[0m"; // cyan: probe in flight
    case ConnState::Verified:
        return "\x1b[1;32m>\x1b[0m"; // bold green: key accepted
    case ConnState::Rejected:
        return "\x1b[1;31m!\x1b[0m"; // bold red: key refused (401/403)
    case ConnState::Unreachable:
        return "\x1b[35mx\x1b[0m"; // magenta: no answer from that endpoint
    case ConnState::Unverified:
        break;
    }
    return "\x1b[33m?\x1b[0m"; // yellow: configured but unprobed
}

std::optional<std::string> slash_tab_complete(std::string_view line, int& cursor_state,
                                              const std::vector<std::string>& session_ids,
                                              const std::vector<std::string>& provider_names,
                                              const std::vector<std::string>& model_choices) {
    if (line.empty() || line.front() != '/')
        return std::nullopt;
    const auto matches = slash_matches(line);
    // v4.10 second tier: a fully-typed command (optionally with a partial
    // single-word argument) cycles its choice list instead.
    {
        std::string_view cmd;
        std::string_view rest;
        if (split_composer(line, &cmd, &rest)) {
            const auto choices = slash_arg_choices(cmd, session_ids, provider_names, model_choices);
            std::vector<std::string_view> hits;
            for (const auto& c : choices)
                if (c.substr(0, rest.size()) == rest)
                    hits.push_back(c);
            const bool arg_mode = line.find(' ') != std::string_view::npos;
            // A fully-typed command enters the arg tier even when longer
            // sibling commands share its prefix ("/session" vs "/sessions",
            // "/model" vs "/models"): the exact word IS a command, so its
            // options outrank the sibling completion.
            const bool exact_cmd =
                !arg_mode && std::find(matches.begin(), matches.end(), line) != matches.end();
            // Args only after the command is unambiguous: a space (partial
            // arg typed) or an exact full command. "/con" Tab still walks
            // the command list.
            if (!hits.empty() && (arg_mode || exact_cmd)) {
                // Rest IS one of the choices (typed or inserted by a
                // previous Tab): advance to the NEXT choice in the full
                // list -- deterministic rotation, cursor not consulted.
                for (std::size_t i = 0; i < choices.size(); ++i) {
                    if (choices[i] == rest) {
                        const std::size_t next = (i + 1) % choices.size();
                        return std::string(cmd) + " " + std::string(choices[next]);
                    }
                }
                // Partial prefix: cycle the filtered hits.
                const std::string_view pick =
                    hits[static_cast<std::size_t>(cursor_state) % hits.size()];
                cursor_state = (cursor_state + 1) % static_cast<int>(hits.size());
                return std::string(cmd) + " " + std::string(pick);
            }
        }
    }
    if (matches.empty())
        return std::nullopt;
    const std::string_view pick = matches[static_cast<std::size_t>(cursor_state) % matches.size()];
    cursor_state = (cursor_state + 1) % static_cast<int>(matches.size());
    return std::string(pick);
}

std::vector<std::string> slash_completion_hint(std::string_view composer,
                                               const std::vector<std::string>& session_ids,
                                               const std::vector<std::string>& provider_names,
                                               const std::vector<std::string>& model_choices) {
    const bool colors = colors_enabled();
    if (composer.empty() || composer.front() != '/')
        return {};
    const auto matches = slash_matches(composer);
    std::vector<std::string> rows;
    // v4.10 second tier: once the composer is an exact command (with or
    // without a partial single-word argument), list that command's
    // argument choices instead of the command list.
    {
        std::string_view cmd;
        std::string_view rest;
        if (split_composer(composer, &cmd, &rest)) {
            const auto choices = slash_arg_choices(cmd, session_ids, provider_names, model_choices);
            std::vector<std::string_view> hits;
            for (const auto& c : choices)
                if (c.substr(0, rest.size()) == rest)
                    hits.push_back(c);
            const bool exact_cmd = rest.empty() && composer.find(' ') == std::string_view::npos &&
                                   std::find(matches.begin(), matches.end(), cmd) != matches.end();
            const bool arg_prefix =
                composer.find(' ') != std::string_view::npos && !choices.empty();
            if (!choices.empty() && (exact_cmd || arg_prefix) && !hits.empty()) {
                rows.emplace_back(
                    tint(colors, "2", "options for " + std::string(cmd) + " (cycle with Tab):"));
                for (const auto& c : hits)
                    rows.push_back(
                        tint(colors, "2", "  " + std::string(cmd) + " " + std::string(c)));
                return rows;
            }
            // No hit under a typed command: free-form values (a token
            // count for /context, a path for /cwd, a key for /login) are
            // legal input, not completion misses -- stay silent instead of
            // nagging. Only a partial that looks like a mistyped option
            // (alphanumeric, command has a fixed choice set, and none of
            // the choices could ever follow) would warrant a notice; the
            // placeholder rows already teach the shape, so we omit it.
        }
    }
    if (matches.empty()) {
        // Past the command word (a space is present) the user is typing a
        // value, not a command: stay silent instead of the no-command
        // notice (pre-v4.10 nag, now visible whenever an arg is typed).
        if (composer.find(' ') != std::string_view::npos)
            return rows;
        rows.emplace_back(tint(colors, "33", "Tab completion: no matching command (see /help)"));
        return rows;
    }
    rows.emplace_back(tint(colors, "2", "Tab completion (cycle with Tab):"));
    for (const auto& cmd : matches)
        rows.push_back(tint(colors, "2", "  " + std::string(cmd)));
    return rows;
}

// ------------------------------------------------------------ v2 tool UI

std::string tool_arg_summary(const std::string& arguments_json) {
    std::string summary;
    // First meaningful fragment of the arguments, sanitized + clipped:
    // never raw control characters reach the frame (008 5.4).
    auto parsed = nlohmann::json::parse(arguments_json, nullptr, false);
    if (parsed.is_object()) {
        // Prefer the obvious primary argument for readability.
        for (const char* key : {"command", "path", "text", "url", "query"}) {
            if (auto it = parsed.find(key); it != parsed.end() && it->is_string()) {
                summary = it->get<std::string>();
                break;
            }
        }
        if (summary.empty())
            summary = parsed.dump();
    } else {
        summary = arguments_json;
    }
    // collapse internal newlines, cap display length
    for (auto& ch : summary)
        if (ch == '\n' || ch == '\r')
            ch = ' ';
    summary = sanitize_display(summary);
    if (summary.size() > 72)
        summary = summary.substr(0, 72) + "...";
    return summary;
}

std::vector<std::string> turn_panel_rows(bool colors, bool thinking, long long elapsed_secs,
                                         const std::vector<std::string>& tool_rows,
                                         std::size_t max_rows) {
    std::vector<std::string> rows;
    rows.push_back(tint(colors, "2", "this turn " + std::to_string(elapsed_secs) + "s"));
    if (thinking)
        rows.push_back(tint(colors, "36", "... thinking"));
    // Newest-last order assumed; drop from the FRONT when over budget so
    // the running/last tool stays visible.
    std::size_t shown = max_rows > 1 ? max_rows - 1 : 0; // header owns one row
    if (tool_rows.size() > shown)
        rows.insert(rows.end(), tool_rows.end() - static_cast<long>(shown), tool_rows.end());
    else
        rows.insert(rows.end(), tool_rows.begin(), tool_rows.end());
    return rows;
}

std::string tool_activity_line(const std::string& tool_name, const std::string& arguments_json,
                               bool executed, bool is_error, long long elapsed_ms,
                               std::size_t result_bytes) {
    const std::string summary = tool_arg_summary(arguments_json);
    // Plain text: `LiveScreen::commit` colours `[tool]` rows by outcome via
    // `style_history_line` (the summary is sanitized, so no ESC bytes reach
    // the classifier's passthrough check).
    std::string line = "[tool] " + sanitize_display(tool_name);
    if (!summary.empty())
        line += " \u00b7 " + summary;
    if (!executed) {
        line += " [denied]";
    } else if (is_error) {
        line += " [error " + std::to_string(elapsed_ms) + "ms \u00b7 " +
                std::to_string(result_bytes) + " B]";
    } else {
        line += " [ok " + std::to_string(elapsed_ms) + "ms \u00b7 " + std::to_string(result_bytes) +
                " B]";
    }
    return line;
}

// v5 (design 011): one colour map for every unified-diff view (the
// approval block and the post-edit transcript block share it). Mirrors
// `git diff` defaults (2026-11-05 maintainer reference screenshot): file
// headers bold meta, hunk header cyan, removed red, added green, context
// left terminal-default. Input must already be sanitized.
std::string tint_diff_line(std::string_view line, bool colors) {
    if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0)
        return tint(colors, "1", line);
    if (!line.empty() && line[0] == '+')
        return tint(colors, "32", line);
    if (!line.empty() && line[0] == '-')
        return tint(colors, "31", line);
    if (!line.empty() && line[0] == '@')
        return tint(colors, "36", line);
    if (!line.empty() && line[0] == ':')
        return tint(colors, "2", line);
    return std::string(line); // context: plain
}

// v5 (design 011): transcript block echoing what a file tool actually
// did AFTER execution -- the applied diff (write_file/edit_file) or the
// file body (preview_file; Markdown rendered, code highlighted). Committed
// right after the [tool] activity line so the user sees edit -> diff ->
// effect in one flow. Producer-styled rows (carry SGR) pass commit()'s
// classifier untouched, same contract as markdown replies.
// `lang` non-empty = highlight the plain body with that language lexer;
// `is_markdown` takes precedence. Diff bodies use the shared colour map.
std::vector<std::string> result_preview_rows(const std::string& tool_name,
                                             const std::string& preview, bool is_markdown,
                                             const std::string& lang, std::size_t cols) {
    std::vector<std::string> rows;
    if (preview.empty())
        return rows;
    const bool colors = colors_enabled();
    if (is_markdown) {
        rows.push_back(tint(colors, "36", "[preview] " + sanitize_display(tool_name)));
        const auto rendered = md::render(preview, cols);
        std::string line;
        std::istringstream stream(rendered);
        while (std::getline(stream, line))
            rows.push_back(line.empty() ? std::string(" ") : line);
        return rows;
    }
    if (!lang.empty()) {
        // v5 5.6: code preview with per-line syntax highlighting (SGR
        // spans are zero-width to md::wrap; the sanitizer already ran).
        rows.push_back(tint(colors, "36", "[preview] " + sanitize_display(tool_name)));
        std::string line;
        std::istringstream stream(preview);
        hl::State state;
        std::size_t shown = 0;
        while (std::getline(stream, line) && shown < 200) {
            const auto lit = hl::highlight_line(sanitize_display(line), lang, state);
            rows.push_back(line.empty() ? std::string(colors ? lit : " ") : lit);
            ++shown;
        }
        if (!stream.eof())
            rows.push_back(tint(colors, "2", "  ... truncated (200 lines)"));
        return rows;
    }
    // Diff body: shared colour map, capped so a huge write cannot flood
    // the transcript.
    std::string line;
    std::istringstream stream(preview);
    std::size_t shown = 0;
    while (std::getline(stream, line) && shown < 40) {
        rows.push_back("  " + tint_diff_line(sanitize_display(line), colors));
        ++shown;
    }
    if (!stream.eof())
        rows.push_back(
            tint(colors, "2", "  ... diff truncated in transcript (full change applied)"));
    return rows;
}

std::optional<ApprovalKey> approval_key(int ch, bool& expanded) noexcept {
    switch (ch) {
    case 'y':
    case 'Y':
        return ApprovalKey::Allow;
    case 'a':
    case 'A':
        return ApprovalKey::AllowSession;
    case 'n':
    case 'N':
        return ApprovalKey::Deny;
    case '?':
        expanded = !expanded;
        return std::nullopt; // toggle only
    default:
        return std::nullopt;
    }
}

std::vector<std::string> approval_rows(const std::string& tool_name,
                                       const std::string& arguments_json, bool expanded,
                                       const std::string& preview) {
    std::vector<std::string> rows;
    const auto parsed = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string primary;
    if (parsed.is_object()) {
        for (const char* key : {"command", "path", "text", "url", "query"}) {
            if (auto it = parsed.find(key); it != parsed.end() && it->is_string()) {
                primary = it->get<std::string>();
                break;
            }
        }
    }
    // Approval block styling (2026-11-04 readability request): the ALLOW
    // lead is bold red (an action gate, not an error), the command summary
    // stays plain and copyable, argument lines dim, the key legend dim.
    const bool colors = colors_enabled();
    rows.push_back(tint(colors, "1;31", "ALLOW " + sanitize_display(tool_name)) +
                   (primary.empty() ? std::string() : ": " + sanitize_display(primary)));
    // v3 (design 009 5.3): the pre-computed diff preview replaces the old
    // "?=params" default view for file writes -- what the human sees IS
    // what will be written (race guard backs the promise up at execute).
    // `?` flips between the diff and the full parameter JSON.
    if (!preview.empty() && !expanded) {
        std::string line;
        std::istringstream stream(preview);
        std::size_t shown = 0;
        while (std::getline(stream, line) && shown < 24) {
            // v5: colour map shared with the post-edit transcript block.
            rows.push_back(tint_diff_line(sanitize_display(line), colors));
            ++shown;
        }
        if (!stream.eof())
            rows.push_back(tint(colors, "2", "  ... diff truncated (approve applies full change)"));
        rows.push_back(tint(colors, "33",
                            "y=allow  n=deny  a=allow-name  ?=params  (Esc/Ctrl-C "
                            "deny+stop)"));
        return rows;
    }
    if (expanded) {
        std::string pretty = parsed.is_discarded() ? arguments_json : parsed.dump(2);
        std::string line;
        std::istringstream stream(pretty);
        int shown = 0;
        while (std::getline(stream, line) && shown < 8) {
            rows.push_back(tint(colors, "2", "  " + sanitize_display(line)));
            ++shown;
        }
        if (!stream.eof())
            rows.push_back(tint(colors, "2", "  ..."));
    }
    rows.push_back(tint(colors, "33",
                        expanded ? "y=allow  n=deny  a=allow-name  ?=params(compact)  "
                                   "other keys held"
                                 : "y=allow  n=deny  a=allow-name  ?=params  "
                                   "(Esc/Ctrl-C deny+stop)"));
    return rows;
}

std::vector<std::string> tool_registry_rows(const std::vector<std::string>& rendered_tools) {
    // Structured dump (2026-11-04 readability request): bold header, tool
    // rows with the name in cyan and the source tag in magenta, so per-tool
    // lines separate visually from headers and legend text. Descriptions
    // (user/server data) stay plain; the ESC bytes here also make
    // style_history_line skip these rows (no double styling).
    const bool colors = colors_enabled();
    std::vector<std::string> rows;
    rows.push_back(tint(colors, "1", "tools (" + std::to_string(rendered_tools.size()) + "):"));
    if (rendered_tools.empty()) {
        rows.push_back(tint(colors, "2", "  (none configured · add mcp_servers to config.json)"));
        return rows;
    }
    for (const auto& tool : rendered_tools) {
        static constexpr std::string_view kSep = " \u00b7 ";
        const auto first = tool.find(kSep);
        const auto second =
            first == std::string::npos ? std::string::npos : tool.find(kSep, first + kSep.size());
        if (colors && first != std::string::npos && second != std::string::npos) {
            rows.push_back(
                "  " + tint(colors, "36", tool.substr(0, first)) + std::string(kSep) +
                tint(colors, "35", tool.substr(first + kSep.size(), second - first - kSep.size())) +
                std::string(kSep) + tool.substr(second + kSep.size()));
        } else {
            rows.push_back("  " + tool);
        }
    }
    return rows;
}

std::string format_token_count(long long n) {
    // <1k raw, <1M one-decimal k, <1G one-decimal M, else one-decimal G
    // (probe sanity caps at 200M, but display stays correct for any size).
    if (n < 1000)
        return std::to_string(n);
    char buf[32];
    if (n < 1000000)
        std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1000.0);
    else if (n < 1000000000)
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
    else
        std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(n) / 1000000000.0);
    return std::string(buf);
}

std::string sidebar_status_glyph(std::string_view xy) {
    // Porcelain "XY" -> one compact glyph. Untracked ??, modified M,
    // added A, deleted D, renamed R, conflicted C; staged+worktree pairs
    // collapse to the strongest signal (the sidebar is a hint list, the
    // git_status tool stays the authoritative view).
    if (xy.size() != 2)
        return {};
    const auto x = xy[0];
    const auto y = xy[1];
    if (x == '?' && y == '?')
        return "??";
    for (const char c : {char('C'), char('D'), char('R'), char('A'), char('M')}) {
        if (x == c || y == c)
            return std::string(1, c);
    }
    return {};
}

std::vector<std::string> compose_columns(const std::vector<std::string>& content,
                                         const std::vector<std::string>& sidebar, std::size_t cols,
                                         std::size_t side_w) {
    std::vector<std::string> out;
    if (side_w == 0 || sidebar.empty() || cols <= side_w + 3)
        return content;                          // no room / disabled: pass through
    const std::size_t split = cols - side_w - 3; // content width + " | "
    const std::size_t n = std::max(content.size(), sidebar.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::string left = i < content.size() ? content[i] : std::string();
        std::string right = i < sidebar.size() ? md::clip(sidebar[i], side_w) : std::string();
        const std::size_t used = md::display_width(left);
        if (used < split)
            left.append(split - used, ' ');
        out.push_back(std::move(left) + " | " + right);
    }
    return out;
}

std::string compose_footer(std::string_view left, std::string_view right, std::size_t cols) {
    if (right.empty())
        return std::string(left);
    const std::size_t rw = md::display_width(right);
    if (rw + 1 >= cols)
        return std::string(right);            // no room for both: the info wins
    const std::size_t budget = cols - rw - 1; // min one-space gutter
    std::string head = md::clip(std::string(left), budget);
    const std::size_t hw = md::display_width(head);
    head.append(cols - rw - hw, ' ');
    return head + std::string(right);
}

std::string sysinfo_line(bool colors, const std::string& user, const std::string& host,
                         const std::string& cwd, const std::string& clock) {
    // v4.12 (design 007 §v4.12): bottom system-info row. Pure so the
    // format is unit-testable; the caller gathers user/host/cwd/clock.
    std::string out = tint(colors, "2",
                           sanitize_display(user) + "@" + sanitize_display(host) + " · " +
                               sanitize_display(cwd) + " · " + sanitize_display(clock));
    return out;
}

std::string status_usage_segments(bool colors, const StatusInputs& inputs) {
    // v4 trailing segments (design 010): ctx reading, out sum, reasoning.
    // Each segment is ` · ` + tinted text; the whole row keeps the
    // pre-v4 shape when every input is unset (returns "").
    std::string out;
    auto segment = [&](const char* sgr, const std::string& text) {
        out += " \u00b7 ";
        out += tint(colors, sgr, text);
    };
    if (inputs.context_tokens > 0) {
        const std::string approx = inputs.context_estimated ? "~" : "";
        if (inputs.context_window > 0) {
            const long long percent = inputs.context_tokens * 100 / inputs.context_window;
            const char* sgr = percent >= 90 ? "31" : (percent >= 70 ? "33" : "2");
            // v4.1: used/max form carries both readings at a glance; the
            // percent lives on as the colour threshold (70% yellow/90% red).
            segment(sgr, "ctx " + approx + format_token_count(inputs.context_tokens) + "/" +
                             format_token_count(inputs.context_window));
        } else {
            segment("2", "ctx " + approx + format_token_count(inputs.context_tokens));
        }
    }
    if (inputs.completion_tokens > 0) {
        segment("2", "out " + format_token_count(inputs.completion_tokens));
    }
    if (!inputs.reasoning_effort.empty() || inputs.reasoning_seen) {
        // v4.2.1: the reading refreshes every successful turn (no sticky
        // "on"). Preference: gateway-echoed level > configured level when
        // this turn's messages proved reasoning > bare "on" when proof
        // exists with no level known > config echo as unconfirmed intent.
        std::string level;
        if (!inputs.reasoning_actual.empty())
            level = inputs.reasoning_actual;
        else if (inputs.reasoning_seen && !inputs.reasoning_effort.empty())
            level = inputs.reasoning_effort; // config confirmed by evidence
        else if (inputs.reasoning_seen)
            level = "on"; // evidence, no level known anywhere
        else
            level = inputs.reasoning_effort; // intent, unconfirmed
        segment("2", "reasoning=" + level);
    }
    return out;
}

std::vector<std::string> reasoning_menu_rows(const std::vector<std::string>& efforts,
                                             std::string_view current,
                                             std::string_view default_effort, bool mandatory) {
    if (efforts.empty())
        return {};
    const bool colors = colors_enabled();
    std::vector<std::string> rows;
    rows.push_back(tint(colors, "1",
                        "reasoning effort (" + std::to_string(efforts.size()) + " supported" +
                            (mandatory ? ", mandatory for this model" : "") + "):"));
    for (std::size_t i = 0; i < efforts.size(); ++i) {
        std::string row = "  " + std::to_string(i + 1) + ". " + efforts[i];
        if (!default_effort.empty() && efforts[i] == default_effort)
            row += "  <- default";
        if (!current.empty() && efforts[i] == current)
            row += "  <- current";
        rows.push_back(row);
    }
    rows.push_back(tint(colors, "2", "  type a number to select; /reasoning off clears"));
    return rows;
}

// ---------------------------------------------------------------- live screen

namespace {

class LiveScreen {
  public:
    // One history row: hard-wrapped by us at `cols` display columns, so
    // cursor accounting never depends on the terminal's own auto-wrap
    // (fixes 2026-11-01 duplicate-row reports: CJK double-width and
    // multi-line replies both desynced the old relative-rows model).
    void commit(std::string_view line) {
        clear_live(); // History colouring (2026-11-04 readability request): classify the
        // tag/role of plain lines centrally, so every caller gets consistent
        // hues without threading a flag through each commit site. Rows that
        // already carry SGR (markdown replies, producer-styled registry/
        // approval/tool rows) pass through untouched -- no double styling.
        // md::wrap treats the spans as zero-width (5.6 rule 2 accounting is
        // unchanged), and commit lines are hard-wrapped, not clipped, so the
        // line may exceed the clipped width of the live status row here.
        const std::string styled = style_history_line(line, colors_enabled());
        const std::size_t cols = current_cols();
        std::size_t printed = 0;
        std::string out;
        for (auto& row : md::wrap(styled, cols)) {
            out += "\x1b[2K" + row + "\n";
            ++printed;
        }
        ::fwrite(out.data(), 1, out.size(), stdout);
        ::fflush(stdout);
        last_frame_.clear();
        prev_rows_.clear();
        inputs_cached_ = false;
        // Bottom-anchor tracking (2026-11-05): commit prints at the block
        // top (clear_live left the cursor there), so the next block starts
        // `printed` rows lower; past the bottom the terminal scrolls and
        // the cursor rests on the last row.
        const std::size_t rows = current_rows();
        block_top_ = std::min(block_top_ + printed, rows > 0 ? rows - 1 : 0);
    }

    // Repaint the live block (pinned question + streaming tail window +
    // slash hint + composer + status).
    // 2026-11-01 "full-terminal refresh": the tail window grows with the terminal
    // height instead of a fixed 6 rows, so streaming output refreshes
    // across (almost) the whole screen and slides like a scroll. Row
    // accounting invariant (5.6 rule 2) holds because the block is
    // capped at rows-1 lines and every frame ends with the cursor one
    // row below the block: the block top can never scroll above screen
    // row 1, so the `\x1b[nA` back-up stays exact and no stale rows
    // survive (shrink below the block is explicitly cleared).
    // 2026-11-01 "pin the question on top while streaming": while streaming, the submitted
    // question is re-shown as the block's first rows (`pinned_question`),
    // because it only enters history at turn commit and would otherwise
    // vanish from the refreshed region.
    void render(const std::string& response_tail, const std::string& pinned_question,
                const std::string& composer, const std::string& composer_display,
                const std::string& prompt, const std::string& status, bool streaming,
                std::size_t cols, const std::vector<std::string>& extra_rows = {},
                const std::string& sysinfo = {}, const std::vector<std::string>& session_ids = {},
                const std::vector<std::string>& sidebar = {},
                std::size_t sidebar_sel = static_cast<std::size_t>(-1),
                const std::vector<std::string>& provider_names = {},
                const std::vector<std::string>& model_choices = {}) { // NOLINT(bugprone-easily-swappable-parameters)
        const std::size_t rows = current_rows();
        // Slash hint (2026-11-01 maintainer request; vertical list per
        // follow-up): while the composer is a slash line, enumerate Tab
        // matches one row each. Derived from the RAW composer -- the
        // display string carries caret/mask decoration that must not
        // affect matching. Empty raw string (masked input) never hints.
        // Pi requestRender pattern (2026-11-01): identical render inputs
        // cannot produce a different frame, so an unchanged poll tick does
        // zero wrap/alloc/compare work. The cache is invalidated by
        // commit()/clear_live(), which move the live block behind our back,
        // and by terminal height changes (window budget follows rows).
        if (inputs_cached_ && rows == last_rows_ && cols == last_cols_ &&
            streaming == last_streaming_ && response_tail == last_tail_ &&
            pinned_question == last_pin_ && composer == last_composer_ &&
            composer_display == last_comp_disp_ && prompt == last_prompt_ &&
            status == last_status_ && extra_rows == last_extra_ && sysinfo == last_sysinfo_ &&
            session_ids == last_session_ids_ && sidebar == last_sidebar_ &&
            sidebar_sel == last_sidebar_sel_ && provider_names == last_provider_names_ &&
            model_choices == last_model_choices_) {
            return;
        }
        inputs_cached_ = true;
        last_rows_ = rows;
        last_cols_ = cols;
        last_streaming_ = streaming;
        last_tail_ = response_tail;
        last_pin_ = pinned_question;
        last_composer_ = composer;
        last_comp_disp_ = composer_display;
        last_prompt_ = prompt;
        last_status_ = status;
        last_extra_ = extra_rows;
        last_sysinfo_ = sysinfo;
        last_session_ids_ = session_ids;
        last_sidebar_ = sidebar;
        last_sidebar_sel_ = sidebar_sel;
        last_provider_names_ = provider_names;
        last_model_choices_ = model_choices;
        // v4.16: one merged footer row (status + right-aligned sysinfo)
        // on top of the cursor row.
        const std::size_t fixed_rows = 2;
        // v4.13: with the sidebar active the content column shrinks to
        // cols - side_w - 3 (" | " gutter); every content row wraps to
        // that width, then composes 1:1 with a sidebar row. Composed rows
        // are exactly one terminal row each (left padded by display
        // width, right clipped), preserving the hard-wrap invariant
        // (5.6 rule 2). Narrow terminals (< 60 cols) shrink the sidebar.
        const std::size_t side_w = sidebar.empty() ? 0 : (cols > 60 ? 30 : cols / 3);
        const std::size_t content_cols = side_w > 0 && cols > side_w + 3 ? cols - side_w - 3 : cols;
        const std::size_t cap = rows > fixed_rows ? rows - fixed_rows : 1;
        auto pin_rows =
            streaming ? md::wrap(style_history_line("You: " + pinned_question, colors_enabled()),
                                 content_cols)
                      : std::vector<std::string>{};
        auto tail_rows =
            streaming ? md::wrap(response_tail, content_cols) : std::vector<std::string>{};
        // The prompt marker carries the provider connection state (2026-11-02
        // request): same row as the composer, one display column wide.
        auto comp_rows = md::wrap(prompt + " " + composer_display, content_cols);
        // Row budget, priority low->high trimmed: status (fixed, appended
        // below), then tail, then the hint list, then the pinned question,
        // keeping the composer whole (2026-11-01: the question stays
        // visible during output, so it outranks the sliding tail window).
        std::size_t budget = cap;
        trim_last(comp_rows, budget);
        budget -= std::min(budget, comp_rows.size());
        trim_last(pin_rows, budget);
        budget -= std::min(budget, pin_rows.size());
        // Vertical list keeps its header when short on rows: build from
        // the top and stop at the budget (opposite of trim_last).
        std::vector<std::string> hint_rows;
        std::vector<std::string> hint_source;
        if (!extra_rows.empty()) {
            hint_source = extra_rows; // approval block outranks slash hints
        } else {
            hint_source = slash_completion_hint(composer, session_ids, provider_names,
                                                model_choices);
        }
        for (const auto& line : hint_source) {
            if (hint_rows.size() >= budget)
                break;
            for (auto& wrapped : md::wrap(line, content_cols)) {
                if (hint_rows.size() >= budget)
                    break;
                hint_rows.push_back(std::move(wrapped));
            }
        }
        budget -= hint_rows.size();
        trim_last(tail_rows, budget);
        std::vector<std::string> content;
        content.insert(content.end(), pin_rows.begin(), pin_rows.end());
        content.insert(content.end(), tail_rows.begin(), tail_rows.end());
        content.insert(content.end(), comp_rows.begin(), comp_rows.end());
        content.insert(content.end(), hint_rows.begin(), hint_rows.end());
        if (side_w > 0) {
            std::vector<std::string> sb = sidebar;
            if (sidebar_sel < sb.size()) // reverse video survives clip (zero-width)
                sb[sidebar_sel] = "\x1b[7m" + sb[sidebar_sel] + "\x1b[27m";
            content = compose_columns(content, sb, cols, side_w);
        }
        // Status is always one row; the trailing reset matters now that the
        // row carries SGR segments: md::clip can cut mid-span, and this is
        // the block's last row, so an unclosed span would leak into the next
        // repaint's rows (5.6 rule 2: no style may survive a row boundary).
        // v4.16: status left + system info right-aligned on ONE row
        // (the two-row footer of v4.12 read as clutter; the info stays
        // fully visible, the status clips with the width budget).
        content.push_back(compose_footer(md::clip(status, cols), md::clip(sysinfo, cols), cols) +
                          "\x1b[0m");
        // Bottom-anchored live block (2026-11-05 maintainer request): pad
        // the TOP with blank rows so the status row rests one line above
        // the screen bottom and the cursor sits on the last line, whether
        // idle or streaming. Padding is dynamic -- only the space between
        // the tracked block top and the bottom is filled, so committed
        // history stays visible until the screen genuinely fills, then the
        // terminal scrolls and we track the shifted top (rule 2 invariant
        // holds: blank rows are plain "\x1b[2K" lines, no styling leaks;
        // the cursor always ends exactly one row below the block).
        if (rows > 1) {
            const std::size_t used = block_top_ + content.size();
            if (used < rows - 1) {
                content.insert(content.begin(), (rows - 1) - used, std::string());
            } else if (used > rows - 1) {
                // Emitting this block scrolled the screen by the overflow:
                // the top moved up accordingly (clamped at row 0).
                const std::size_t scrolled = used - (rows - 1);
                block_top_ = block_top_ > scrolled ? block_top_ - scrolled : 0;
            }
        }

        const std::string joined = join(content);
        if (joined == last_frame_ && content.size() == live_rows_) {
            return; // nothing changed: skip redundant repaint
        }
        last_frame_ = joined;

        // Row-level diff (Pi TuiMainScreen pattern, 2026-11-01): rows
        // identical to the previous frame at the same block index are left
        // untouched (each row is reset-terminated by md::wrap, so skipping a
        // row cannot leak styling). Only the changed prefix onward is
        // rewritten; the shrink cleanup below stays byte-identical to the
        // pre-diff tested path, so the hard-wrap row accounting of rule 2 is
        // not re-opened to the duplicate-row class of bugs.
        std::size_t first_changed = 0;
        while (first_changed < content.size() && first_changed < prev_rows_.size() &&
               content[first_changed] == prev_rows_[first_changed]) {
            ++first_changed;
        }

        std::string out = "\x1b[?2026h"; // synchronized output: atomic frame
        // CSI 0 A means one row up, not zero: a new block must start at
        // the current cursor or its first repaint erases committed history.
        if (live_rows_ > 0)
            out += "\x1b[" + std::to_string(live_rows_) + "A";
        if (first_changed > 0) {
            out += "\x1b[" + std::to_string(first_changed) + "B";
        }
        out += "\r";
        for (std::size_t i = first_changed; i < content.size(); ++i) {
            out += "\x1b[2K" + content[i] + "\n";
        }
        if (live_rows_ > content.size()) {
            // Block shrank (e.g. multi-line composer cleared on Enter):
            // erase the stale rows left below the new block, then step
            // back to the line under its end.
            const std::size_t extra = live_rows_ - content.size();
            for (std::size_t i = 0; i < extra; ++i)
                out += "\x1b[2K\n";
            out += "\x1b[" + std::to_string(extra) + "A";
        }
        out += "\x1b[?2026l";
        ::fwrite(out.data(), 1, out.size(), stdout);
        ::fflush(stdout);
        live_rows_ = content.size();
        prev_rows_ = std::move(content);
    }

    void clear_live() {
        inputs_cached_ = false;
        if (live_rows_ == 0) {
            prev_rows_.clear();
            last_frame_.clear();
            return;
        }
        std::string out = "\x1b[" + std::to_string(live_rows_) + "A";
        for (std::size_t i = 0; i < live_rows_; ++i)
            out += "\x1b[2K\n";
        out += "\x1b[" + std::to_string(live_rows_) + "A";
        ::fwrite(out.data(), 1, out.size(), stdout);
        ::fflush(stdout);
        live_rows_ = 0;
        last_frame_.clear();
        prev_rows_.clear();
    }

  private:
    static std::size_t current_cols() {
        const int c = term_columns();
        return c > 0 ? static_cast<std::size_t>(c) : 80;
    }

    static std::size_t current_rows() {
        const int r = term_rows();
        return r > 0 ? static_cast<std::size_t>(r) : 24;
    }

    // Keep the last `keep` rows of a wrapped block (window slide).
    static void trim_last(std::vector<std::string>& v, std::size_t keep) {
        if (v.size() > keep) {
            v.erase(v.begin(), v.end() - static_cast<std::ptrdiff_t>(keep));
        }
    }

    std::size_t live_rows_ = 0;
    // Screen row (0-based) where the live block starts. Tracked across
    // commits (each commit prints at the block top and advances it) and
    // scrolls (render() subtracts the overflow the emit caused), so
    // render() can pad the top and keep the status row bottom-anchored
    // (2026-11-05). Terminal resize is self-correcting: a smaller height
    // makes the next frame see overflow and clamp the top back down; a
    // taller one just pads more. After a resize the exact anchor may be
    // off until the next commit re-advances the top (terminal reflow
    // moves the real cursor in ways we cannot compute without a cursor
    // position report); the block stays fully visible and correct either
    // way -- padding only shifts where the block starts, never what it
    // erases (live_rows_ accounting is independent).
    std::size_t block_top_ = 0;
    std::string last_frame_;             // repaint suppression cache; reset on commit/clear
    std::vector<std::string> prev_rows_; // previous frame's rows for the diff

    // render() input cache (Pi requestRender pattern): skip all work when
    // the poll tick produced identical inputs. Invalidated on every
    // commit()/clear_live() (block moved) and on rows/cols/streaming change.
    bool inputs_cached_ = false;
    std::size_t last_rows_ = 0;
    std::size_t last_cols_ = 0;
    bool last_streaming_ = false;
    std::string last_tail_;
    std::string last_pin_;
    std::string last_composer_;
    std::string last_comp_disp_;
    std::string last_prompt_;
    std::string last_status_;
    std::vector<std::string> last_extra_;
    std::string last_sysinfo_;
    std::vector<std::string> last_session_ids_;
    std::vector<std::string> last_sidebar_;
    std::size_t last_sidebar_sel_ = static_cast<std::size_t>(-1);
    std::vector<std::string> last_provider_names_;
    std::vector<std::string> last_model_choices_;

    static std::string join(const std::vector<std::string>& rows) {
        std::string out;
        for (const auto& r : rows) {
            out += r;
            out += '\x01';
        }
        return out;
    }
};

// LiveScreen is declared inside an anonymous namespace above; the free
// helpers need to be visible before it, so keep this region closed here.

// Split rendered markdown (may itself contain ANSI) into one commit per
// logical line, prefixing only the first line of the assistant block.
std::vector<std::string> split_ansi_paragraphs(const std::string& rendered) {
    std::vector<std::string> lines;
    std::istringstream stream(rendered);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        // "AI: " prefix (4 display cols); continuation rows indent to align.
        lines.push_back(first ? "AI: " + line : "    " + line);
        first = false;
    }
    if (lines.empty())
        lines.push_back("AI: ");
    return lines;
}

std::chrono::steady_clock::time_point steady_now() {
    return std::chrono::steady_clock::now();
}

} // namespace

// ---------------------------------------------------------------- main loop

int run_loop(AppConfig& config, Session& session, SessionStore& store, const TuiOptions& options) {
    RawMode raw(STDIN_FILENO);
    std::signal(SIGINT, SIG_IGN); // Ctrl-C must arrive as byte 0x03, not a signal

    LiveScreen screen;
    auto cols_now = [] {
        const int c = term_columns();
        return c > 0 ? static_cast<std::size_t>(c) : 80u;
    };
    TurnBridge bridge;
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> turn_done{false};
    bool exiting{false};
    TurnResult result;
    std::thread worker;
    std::string composer;
    std::size_t cursor = 0; // caret byte offset in composer (codepoint boundary)
    std::string last_user;
    std::vector<std::string> pending; // Pi-style Enter queue (5.6 v1.x #1)
    // Single-line ↑/↓ history (2026-11-01 arrow-key request): hist_pos ==
    // history.size() means the draft slot (what the user is typing now).
    // Seeded from the restored session so --session resumes recall too.
    std::vector<std::string> history;
    for (const auto& m : session.chat.messages())
        if (m.role == Role::User && !m.content.empty() && m.content.front() != '/')
            history.push_back(m.content);
    std::size_t hist_pos = history.size();
    std::string history_draft;
    auto make_logger = [&config] {
        return std::make_unique<Logger>(
            Logger::resolve_path(config).value_or(std::filesystem::path{}));
    };
    auto logger = make_logger();
    // v4.9: /session switches the live session in-process, so the banner/
    // status id is a mutable copy of the launch id, not TuiOptions itself.
    std::string active_session_id = options.session_id;
    // v4.15: temp mode is mutable too. `/session new` from a temp session
    // keeps it temp; `/session <id>` resumes a saved session and ends it
    // (the target is a normal, persisted session).
    bool temp_mode = options.temp;
    logger->event("session_open", {{"mode", "tui"},
                                   {"id", active_session_id},
                                   {"model", config.model},
                                   {"log", logger->enabled() ? "on" : "off"}});
    bool secret_input = false; // /login: mask composer echo
    std::string secret_field;  // target key when secret_input
    // Model discovery (2026-11-02 request): a separate short-lived worker
    // queries GET {base_url}/models so the frame loop never blocks on it.
    // The result is read only after join(), mirroring `result`/`turn_done`.
    std::thread models_worker;
    std::atomic<bool> models_done{false};
    bool models_running = false;
    struct ModelsResult {
        bool ok = false;
        std::vector<std::string> ids;
        std::vector<ModelCapabilities> caps; // v4.2: same GET, richer parse
        std::string error;
        int http_status = 0; // 0 = no response arrived (auth unverifiable)
        bool network_error = false;
        // v6 (design 012 §5.2): one sub-result per fetch target. The unnamed
        // top-level config is the empty-name entry; each named profile gets
        // its own GET. The fields above hold the ACTIVE profile's sub-result
        // (legacy single-profile folds read them unchanged).
        struct Part {
            std::string name; // "" = unnamed top-level config
            bool ok = false;
            std::vector<std::string> ids;
            std::vector<ModelCapabilities> caps;
            std::string error;
            int http_status = 0;
            bool network_error = false;
        };
        std::vector<Part> parts;
    } models_result;
    AuthProbe auth; // ACTIVE profile's verdict, shown by /config too
    // v6: per-profile verdicts survive provider switches ("" = unnamed).
    // `auth` always mirrors the active profile's map entry.
    std::map<std::string, AuthProbe> auth_probes;
    std::string models_fingerprint; // every target's name+url+key, '\n'-joined (not a hash)
    std::chrono::steady_clock::time_point models_start;
    bool model_menu_open = false;
    std::vector<std::string> model_choices; // display labels backing the open menu
    // v6: selection target for each model_choices row (provider "" = keep
    // the active profile). Parallel to model_choices, same lifecycle.
    std::vector<std::pair<std::string, std::string>> model_choice_targets;
    // v4.3 (design 010 §13): one-shot context-window probe worker, same
    // join-then-read discipline as models_worker (screen/logger stay on
    // the main thread).
    std::thread probe_worker;
    std::atomic<bool> probe_done{false};
    bool probe_running = false;
    struct ProbeResult {
        ProbeOutcome outcome; // v4.3.1: kind matters (window vs output cap)
        std::string error;    // network/config failure text
    } probe_result;
    // v4.2 (design 010 §11): capability metadata from the last successful
    // /models fetch (main-thread only; the worker fills models_result.caps).
    std::vector<ModelCapabilities> model_caps;
    bool reasoning_menu_open = false;
    std::vector<std::string> reasoning_choices; // efforts backing the open menu
    int tab_state = 0;
    // v4.13 (maintainer request): session-id snapshot for /session Tab
    // completion + prefix resolution. Readdir is cheap but not free, so it
    // refreshes on edges only: entering/leaving the /session arg tier, and a
    // completed turn (the save that creates the file). Also part of render()'s
    // input cache, so a refresh legitimately yields a new frame.
    std::vector<std::string> session_ids;
    bool session_ids_stale = true;
    auto refresh_session_ids = [&store, &session_ids, &session_ids_stale] {
        std::string err;
        session_ids = store.list(&err); // empty on error: hints just get shorter
        session_ids_stale = false;
    };
    bool streaming = false;
    std::size_t last_prompt_chars = 0;
    // v4 status-bar state: usage readings folded from successful turns
    // (main-thread only, written after worker join). When no provider ever
    // reported usage, the ctx reading falls back to a character estimate
    // of the outgoing chat, recomputed lazily at turn boundaries.
    long long status_context_tokens = 0;
    long long status_completion_tokens = 0;
    bool status_context_estimated = false;
    bool status_usage_reported = false;
    long long estimated_chat_tokens = 0;
    bool estimate_dirty = true;
    // v4.2: reasoning ACTUALLY observed in responses (status preference).
    bool status_reasoning_seen = false;
    std::string status_reasoning_actual;
    // v6 (design 012): profile names for /login and /provider completion.
    auto provider_name_list = [&] {
        std::vector<std::string> names;
        names.reserve(config.providers.size());
        for (const auto& p : config.providers)
            names.push_back(p.name);
        return names;
    };
    // v4.2: cached capability lookup for the current model (empty when the
    // gateway exposed no metadata or the model is not in the list).
    auto caps_for_model = [&]() -> ModelCapabilities {
        for (const auto& cap : model_caps)
            if (cap.id == config.model)
                return cap;
        return {};
    };
    auto context_reading = [&]() -> StatusInputs {
        StatusInputs inputs;
        inputs.context_window = config.context_window;
        // v4.2.1: config stays authoritative, but an unset window falls
        // back to the gateway's advertised context_length (cached caps)
        // so the ctx reading gets its /max denominator without extra
        // config work on OpenRouter-style gateways.
        if (inputs.context_window <= 0)
            inputs.context_window = caps_for_model().context_length;
        // v4.7 (design 010 §18): pi-style static catalog last -- gateways
        // without context_length metadata (DashScope token-plan) still get
        // a denominator when base_url+model match a models.dev row. The
        // catalog never overwrites config: it only fills the display gap.
        if (inputs.context_window <= 0) {
            if (const auto limits = lookup_models_dev(config.base_url, config.model))
                inputs.context_window = limits->context;
        }
        inputs.reasoning_effort = config.reasoning_effort;
        inputs.completion_tokens = status_completion_tokens;
        inputs.reasoning_seen = status_reasoning_seen;
        inputs.reasoning_actual = status_reasoning_actual;
        if (status_usage_reported && status_context_tokens > 0) {
            inputs.context_tokens = status_context_tokens;
            inputs.context_estimated = status_context_estimated;
        } else {
            if (estimate_dirty) {
                estimated_chat_tokens = estimate_context_tokens(chat_to_json(session.chat));
                estimate_dirty = false;
            }
            inputs.context_tokens = estimated_chat_tokens;
            inputs.context_estimated = true;
        }
        return inputs;
    };
    // v4: switching models invalidates every usage reading (the window and
    // all prior counts belong to the old route).
    auto reset_usage_readings = [&] {
        status_context_tokens = 0;
        status_completion_tokens = 0;
        status_usage_reported = false;
        status_context_estimated = false;
        estimate_dirty = true;
        status_reasoning_seen = false; // v4.2: evidence belongs to the old model
        status_reasoning_actual.clear();
        reasoning_menu_open = false;
        reasoning_choices.clear();
    };
    // v6 (design 012 §5.4): provider switching. `auth` always mirrors the
    // active profile's map entry; per-profile verdicts survive the switch.
    // The capability cache is invalidated (it belongs to the old route) and
    // every usage reading resets, exactly like a cross-route model change.
    auto switch_active_profile = [&] {
        auth = auth_probes[config.active_provider]; // default-constructed on first sight
        model_caps.clear();
        reset_usage_readings();
    };
    auto turn_start = steady_now();
    auto last_ctrl_c = std::chrono::steady_clock::time_point{};

    const auto banner_model = config.model.empty() ? std::string("(no model)") : config.model;
    // v4.15: tag the id with how it was chosen (temp | resumed | new).
    const char* session_tag = temp_mode ? "temp" : (options.resumed ? "resumed" : "new");
    screen.commit("mini-agent · model=" + banner_model + " · session=" + active_session_id + " (" +
                  session_tag + ")");
    if (!config.complete()) {
        // Names only the next missing step: the three commands save
        // independently, so they may be supplied one per sitting.
        screen.commit("[warn] LLM not configured · " + config_next_hint(config) +
                      " (env vars also work)");
    } else {
        // Fully configured from a previous run: the stored key has not been
        // checked against the provider yet in this process.
        screen.commit(auth_status_line(auth));
    }
    screen.commit("Enter sends; Enter while streaming queues; Ctrl-C/Esc interrupt; "
                  "Ctrl-C twice quits; /help; Tab completes");

    // v2 tool layer (design 008 section 5.4): registry, approval bridge.
    std::string tool_note;
    auto tool_registry = build_tool_registry(config, &tool_note);
    // v4.11 turn panel (design 010 §21): live process rows during a turn.
    // `running_tool` is set by on_tool_start (worker thread) and cleared
    // by on_tool -- both through TurnBridge's tool queue; `reasoning_live`
    // is set by the SSE parser the first time a reasoning delta arrives.
    bool show_sysinfo = true; // v4.12: /sysinfo on|off toggle
    // v4.13: right sidebar (workspace tree + changed files). Data comes
    // from a short-lived worker (fs walk + `git status --porcelain`): the
    // frame loop must not block, same join-then-read discipline as the
    // models fetch. Refresh triggers: /sidebar on, turn completion, /cwd,
    // /session switch.
    bool show_sidebar = false;
    std::thread sidebar_worker;
    std::atomic<bool> sidebar_done{false};
    bool sidebar_running = false;
    bool sidebar_stale = false; // refresh requested while a scan ran
    struct SidebarResult {
        std::vector<std::string> rows; // pre-tinted? no: plain, render tints
        // Parallel to rows: selectable entries carry the ABSOLUTE path;
        // headers/blanks are empty (skipped by the focus navigation).
        std::vector<std::string> paths;
        std::string error; // workspace unreadable etc.
    } sidebar_result;
    // v4.14: sidebar focus mode (Ctrl-F): arrow keys walk selectable rows,
    // Enter inserts the file path into the composer, Esc leaves. Index is
    // into rows/paths; navigation skips non-selectable entries.
    bool sidebar_focus = false;
    std::size_t sidebar_sel = 0;
    std::atomic<bool> reasoning_live{false};
    // Completed tool rows for THIS turn, kept for the panel even after
    // drain_tool_events commits them to history (the panel is the turn's
    // process view; history scrolls away).
    std::vector<std::string> panel_tool_rows;
    std::atomic<bool> approval_abort{false};
    bool approval_expanded = false;
    std::set<std::string> session_allowed_tools;
    std::mutex session_allowed_mutex; // worker memoizes AllowSession names
    // Allowlist names are snapshotted per turn like the rest of config;
    // /tools and slash handlers rebuild both the registry and this copy.
    AppConfig config_snapshot = config;
    const std::size_t max_tool_rounds = config.max_tool_rounds;
    bool tools_active = false; // any tool round ran in the current turn
    if (!tool_note.empty())
        screen.commit("[info] " + tool_note + " · lazy start on first call · /tools");
    std::string approval_policy = // mutable via /approve while worker idle
        config.tool_approval.empty() ? std::string("ask") : config.tool_approval;
    if (approval_policy == "off" && !tool_registry->empty()) {
        screen.commit("[warn] tool_approval=off: tools run WITHOUT asking");
        logger->event("tools", {{"status", "auto"}, {"approval", "off"}});
    }
    logger->event("tools", {{"status", "ready"},
                            {"approval", approval_policy},
                            {"servers", std::to_string(config.mcp_servers.size())},
                            {"builtin", tool_registry->empty() ? "no" : "yes"}});

    auto drain_tool_events = [&] {
        for (const auto& round : bridge.drain_tool_events()) {
            screen.commit(tool_activity_line(round.tool_name, round.arguments_json, round.executed,
                                             round.is_error, round.elapsed_ms, round.result_bytes));
            // v5 (design 011): echo the applied diff / preview body right
            // after the activity row -- edit -> diff -> effect in one
            // transcript flow.
            for (const auto& row :
                 result_preview_rows(round.tool_name, round.result_preview,
                                     round.preview_is_markdown, round.preview_lang, cols_now()))
                screen.commit(row);
            // v4.11: the panel keeps the whole turn's rows (history scrolls).
            panel_tool_rows.push_back(tool_activity_line(round.tool_name, round.arguments_json,
                                                         round.executed, round.is_error,
                                                         round.elapsed_ms, round.result_bytes));
            tools_active = true;
        }
    };

    auto approval_pending = [&] {
        return bridge.approval_pending();
    };
    // Answer the pending approval with the UI key semantics (008 5.4):
    // y allow once, n deny, a allow-name for the session, Esc/Ctrl-C deny.
    auto answer_approval = [&](int key) {
        ToolCall call = bridge.approval_call(); // snapshot under the slot mutex
        int answer = 2;
        bool handled = true;
        switch (key) {
        case 'y':
        case 'Y':
            answer = 1;
            break;
        case 'a':
        case 'A':
            answer = 3;
            break;
        case 'n':
        case 'N':
            answer = 2;
            break;
        case '?':
            approval_expanded = !approval_expanded;
            handled = false;
            break;
        default:
            handled = false; // other keys are held (swallowed) while pending
            break;
        }
        if (!handled)
            return;
        bridge.answer_approval(answer);
        logger->event("tool_approve",
                      {{"tool", call.name},
                       {"decision", answer == 1 ? "allow" : (answer == 3 ? "session" : "deny")}});
    };

    auto start_turn = [&](const std::string& text) {
        if (worker.joinable())
            worker.join();
        config_snapshot = config; // refresh after join, before worker start
        last_user = text;
        bridge.reset_turn();
        abort_requested = false;
        turn_done = false;
        result = TurnResult{};
        reasoning_live = false; // v4.11 panel evidence belongs to the new turn
        panel_tool_rows.clear();
        approval_abort = false;
        approval_expanded = false;
        tools_active = false;
        turn_start = steady_now();
        streaming = true;
        last_prompt_chars = text.size();
        Message user;
        user.role = Role::User;
        user.content = text;
        // Snapshot config for the worker; the provider must be constructed
        // INSIDE the thread (start_turn returns before the worker runs).
        // Intentional snapshot: config mutates on the main thread while the
        // worker runs (/login, /model); the copy is the thread boundary.
        const AppConfig turn_config = config; // NOLINT(performance-unnecessary-copy-initialization)
        worker = std::thread([&, turn_config, user_msg = std::move(user)]() mutable {
            auto provider_config = turn_config.to_provider();
            provider_config.reasoning_live = &reasoning_live; // v4.11 panel
            OpenAiProvider provider(std::move(provider_config));
            // v4.17: the old second deep copy (`Chat outgoing`) was dead
            // code -- run_turn builds and commits its own `turn_chat`
            // below; one snapshot per turn is the thread boundary.
            TurnResult r;
            // Snapshot of the shared control state for this turn's tools
            // (thread-boundary discipline mirrors turn_config; the TUI
            // rebuilds the registry before every start_turn, so mutations
            // of `tool_registry` here are serialized by the worker join).
            ToolRegistry* registry = tool_registry.get();
            TurnOptions turn_options;
            turn_options.tools = registry;
            turn_options.max_tool_rounds = max_tool_rounds;
            turn_options.should_abort = [&] {
                return abort_requested.load() || approval_abort.load();
            };
            turn_options.approve = [&](const ToolCall& call) -> Approval {
                const bool auto_run = approval_policy == "off" ||
                                      (approval_policy == "allowlist" &&
                                       std::find(config_snapshot.tool_allowlist.begin(),
                                                 config_snapshot.tool_allowlist.end(), call.name) !=
                                           config_snapshot.tool_allowlist.end());
                if (auto_run || session_allowed_tools.count(call.name) != 0)
                    return Approval::Allow;
                unsigned long long seq = 0;
                // v3: compute the diff preview on the worker thread
                // (registry preview honors the same single-owner
                // contract as list/call) and arm the runtime's race
                // guard -- both before publish. publish_approval stamps
                // the countdown source and publishes request_seq (v4.17:
                // protocol lives in TurnBridge).
                seq = bridge.publish_approval(
                    call, registry ? registry->preview(call) : std::string());
                const auto wait =
                    bridge.wait_approval(seq, kApprovalTimeoutS, [&] {
                        return abort_requested.load();
                    });
                if (wait == ApprovalWait::Timeout) { // never block the worker forever
                    ToolRound timeout_round;
                    timeout_round.tool_name = call.name;
                    timeout_round.arguments_json = call.arguments_json;
                    timeout_round.executed = false;
                    timeout_round.is_error = true;
                    timeout_round.approval_timeout = true;
                    bridge.push_tool_event(std::move(timeout_round));
                }
                const int answer = bridge.approval_answer(seq);
                switch (answer) {
                case 1:
                    return Approval::Allow;
                case 3: {
                    std::lock_guard<std::mutex> lock(session_allowed_mutex);
                    session_allowed_tools.insert(call.name);
                    return Approval::AllowSession;
                }
                default:
                    // v3: denial must release any preview-armed race guard
                    // (worker owns the registry; preview() armed it here).
                    if (registry)
                        registry->disarm(call);
                    return Approval::Deny;
                }
            };
            turn_options.on_tool_start = [&](const ToolCall& call) {
                // v4.11: panel shows the call as running until on_tool
                // lands (approval-gated calls fire this only after the
                // approve decision, denied calls never fire it).
                bridge.set_running_tool("[tool] " + sanitize_display(call.name) +
                                        (tool_arg_summary(call.arguments_json).empty()
                                             ? std::string()
                                             : " · " + tool_arg_summary(call.arguments_json)) +
                                        " [running]");
            };
            turn_options.on_tool = [&](const ToolCall& call, const ToolResult& tool_result,
                                       bool executed) {
                ToolRound round;
                round.tool_name = call.name;
                round.arguments_json = call.arguments_json;
                round.executed = executed;
                round.is_error = tool_result.is_error;
                round.elapsed_ms = tool_result.elapsed_ms;
                round.result_bytes = tool_result.text.size();
                // v5 (design 011): stage the human preview on the worker
                // (loop) thread right after execution -- the registry's
                // result_preview shares call()'s single-owner contract.
                if (executed && !tool_result.is_error && registry) {
                    round.result_preview = registry->result_preview(call, tool_result);
                    if (call.name == "preview_file") {
                        // .md/.markdown bodies render; others show verbatim.
                        // The path lives in the args; parse it for the
                        // extension (staged body carries no name).
                        const auto args =
                            nlohmann::json::parse(call.arguments_json, nullptr, false);
                        std::string path =
                            args.is_object() && args.contains("path") && args["path"].is_string()
                                ? args["path"].get<std::string>()
                                : std::string();
                        std::string lower = path;
                        for (auto& c : lower)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        const auto ends = [&](std::string_view suf) {
                            return lower.size() >= suf.size() &&
                                   lower.compare(lower.size() - suf.size(), suf.size(), suf) == 0;
                        };
                        round.preview_is_markdown = ends(".md") || ends(".markdown");
                        // v5 5.6: non-markdown recognized code files
                        // highlight; markdown takes precedence above.
                        if (!round.preview_is_markdown)
                            round.preview_lang = hl::language_for_path(path);
                    }
                }
                bridge.finish_running_tool(std::move(round));
            };
            // v4: run_turn writes here only on a successful turn; the
            // worker copies it into `result` (join-then-read discipline).
            TokenUsage turn_usage;
            turn_options.usage_out = &turn_usage;
            ActualReasoning turn_reasoning; // v4.2
            turn_options.reasoning_out = &turn_reasoning;
            try {
                // v4.17 (copy-on-write thread boundary): run_turn takes
                // the stored chat by reference and commits the whole turn
                // to it ONLY on success -- the loop's internal staging
                // copy is the error isolation (5.6 rule: failures never
                // reach history; 008 section 4). The worker's former deep
                // copy of the entire history was redundant defense and
                // made every turn O(chat) twice; the CLI path already
                // passes session.chat directly. Main-thread reads of the
                // chat during the turn are gated exactly as before: the
                // only writer window is the commit at turn end, and the
                // same-turn success write happened in the old design too.
                r.reply = run_turn(
                    session.chat, provider, user_msg.content,
                    [&](std::string_view d) { bridge.append_delta(d); },
                    [&] { return abort_requested.load(); }, turn_options);
                r.usage = turn_usage;
                r.reasoning = turn_reasoning;
                r.ok = true;
            } catch (const ProviderError& e) {
                r.error = e.what();
                r.http_status = e.http_status();
                r.network_error = e.kind() == ProviderErrorKind::Network;
            } catch (const std::exception& e) {
                r.error = e.what();
            }
            if (abort_requested.load())
                r.aborted = true;
            result = std::move(r);
            turn_done = true;
        });
    };

    auto save_and_report = [&](bool show_progress = false) {
        std::string err;
        if (save_config_file(config, &err)) {
            screen.commit("[ok] config saved (api_key stored in config file, 0600)");
            // Batched entry: after each independently-saved piece, name only
            // what is still missing so the user knows this step landed and
            // what to supply next (possibly in a later session).
            if (show_progress)
                screen.commit(config_next_hint(config));
        } else {
            screen.commit("[warn] config save failed: " + err);
        }
    };

    // Query GET {base_url}/models off-thread (frame loop must not block).
    // v6 (design 012 §5.3): ONE aggregate job per fetch. Every complete
    // profile (url+key set) gets its own sequential GET -- partial profiles
    // are skipped until /login completes them -- plus the unnamed top-level
    // values when they are set and not already covered by a profile (the
    // env-override case). Per-profile failure never blocks the others.
    // Logs only metadata (status/count/elapsed): api_keys never reach the
    // log line, the menu, or any ProviderError text.
    auto start_sidebar = [&] {
        if (!show_sidebar)
            return;
        if (sidebar_running) {
            // A scan is in flight (git status on a big repo can take
            // seconds): NEVER join on the main thread -- that would stall
            // the frame loop (v4.14 regression: cascading smoke failures
            // from a blocked poll tick). Mark stale; the completion
            // handler re-fires once the in-flight scan lands.
            sidebar_stale = true;
            return;
        }
        if (sidebar_worker.joinable())
            sidebar_worker.join(); // previous scan is done: cheap join
        sidebar_result = SidebarResult{};
        sidebar_done = false;
        sidebar_running = true;
        const std::string root =
            config.workspace.empty() ? std::filesystem::current_path().string() : config.workspace;
        sidebar_worker = std::thread([&, root] {
            SidebarResult r;
            try {
                std::error_code ec;
                const std::filesystem::path base(root);
                if (!std::filesystem::is_directory(base, ec)) {
                    r.error = "not a directory: " + root;
                } else {
                    auto run_git_status = [&]() -> std::vector<std::string> {
                        // Alternating [display, path, display, path, ...].
                        std::vector<const char*> argv = {"git",
                                                         "-C",
                                                         root.c_str(),
                                                         "--no-pager",
                                                         "--no-optional-locks",
                                                         "status",
                                                         "--porcelain=v1",
                                                         nullptr};
                        SubprocessSpec spec;
                        spec.exec_path = "git";
                        spec.argv = &argv;
                        spec.workspace = root;
                        spec.timeout_seconds = 5;
                        auto out = run_subprocess(spec, [] { return false; });
                        std::vector<std::string> lines;
                        std::istringstream in(out.text);
                        std::string line;
                        while (std::getline(in, line)) {
                            std::string xy, path;
                            if (line.size() < 4)
                                continue;
                            std::string code = line.substr(0, 2);
                            std::string file = line.substr(3);
                            const auto glyph = sidebar_status_glyph(code);
                            if (!glyph.empty()) {
                                lines.push_back(glyph + " " + file);
                                lines.push_back(file);
                            }
                        }
                        return lines;
                    };
                    // rows and paths grow in lockstep (v4.14 selection):
                    // paths[i] is workspace-relative for selectable entries,
                    // empty for headers/captions (navigation skips those).
                    std::vector<std::string> rows;
                    std::vector<std::string> paths;
                    auto add = [&](std::string text, std::string path) {
                        rows.push_back(std::move(text));
                        paths.push_back(std::move(path));
                    };
                    std::function<void(const std::filesystem::path&, int, std::size_t&)> walk =
                        [&](const std::filesystem::path& dir, int depth, std::size_t& count) {
                            if (depth > 2 || rows.size() >= 18 || count > 400)
                                return;
                            std::vector<std::string> names;
                            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                                if (ec)
                                    break;
                                const std::string name = e.path().filename().string();
                                if (name.front() == '.' || name == "node_modules" ||
                                    name == "build" || name == "__pycache__" || name == "target" ||
                                    name == "venv")
                                    continue;
                                names.push_back(name);
                                ++count;
                            }
                            std::sort(names.begin(), names.end());
                            for (const auto& name : names) {
                                if (rows.size() >= 18)
                                    break;
                                const auto child = dir / name;
                                const bool is_dir = std::filesystem::is_directory(child, ec);
                                const auto rel = std::filesystem::relative(child, base, ec);
                                const std::string rel_str = ec ? std::string() : rel.string();
                                add(std::string(static_cast<std::size_t>(depth) * 2, ' ') +
                                        (is_dir ? name + "/" : name),
                                    rel_str);
                                if (is_dir)
                                    walk(child, depth + 1, count);
                            }
                        };
                    std::size_t count = 0;
                    add(sanitize_display(std::filesystem::path(root).filename().empty()
                                             ? root
                                             : std::filesystem::path(root).filename().string()),
                        std::string()); // header row: not selectable
                    walk(base, 1, count);
                    if (count > 400)
                        add("... (large tree, listing capped)", std::string());
                    r.rows = rows;
                    r.paths = paths;
                    auto changed_pairs = run_git_status(); // pairs: display, path
                    if (!changed_pairs.empty()) {
                        r.rows.push_back("");
                        r.paths.push_back("");
                        r.rows.push_back("changed:");
                        r.paths.push_back("");
                        for (std::size_t i = 0; i + 1 < changed_pairs.size(); i += 2) {
                            if (r.rows.size() >= 26) {
                                r.rows.push_back("  ...");
                                r.paths.push_back("");
                                break;
                            }
                            r.rows.push_back("  " + changed_pairs[i]);
                            r.paths.push_back(changed_pairs[i + 1]);
                        }
                    }
                }
            } catch (const std::exception& e) {
                r.error = e.what();
            } catch (...) {
                r.error = "sidebar scan failed";
            }
            sidebar_result = std::move(r);
            sidebar_done = true;
        });
    };

    auto start_models_fetch = [&](bool force) {
        if (models_running)
            return;
        // Fetch targets: name -> (base_url, api_key). Empty name = unnamed.
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> targets;
        std::string fingerprint;
        for (const auto& profile : config.providers) {
            fingerprint += profile.name + '\n' + profile.base_url + '\n' + profile.api_key + '\n';
            if (!profile.base_url.empty() && !profile.api_key.empty())
                targets.emplace_back(profile.name,
                                     std::make_pair(profile.base_url, profile.api_key));
        }
        if (!config.base_url.empty() && !config.api_key.empty()) {
            // The unnamed values duplicate a profile when some profile has
            // the same URL (the active expansion always does; only an env
            // override points somewhere else) -- fetch that route once.
            bool covered = false;
            for (const auto& profile : config.providers)
                if (profile.base_url == config.base_url)
                    covered = true;
            fingerprint += '\n' + config.base_url + '\n' + config.api_key + '\n';
            if (!covered)
                targets.emplace_back(std::string(),
                                     std::make_pair(config.base_url, config.api_key));
        }
        if (targets.empty())
            return;
        if (!force && fingerprint == models_fingerprint)
            return;
        if (models_worker.joinable())
            models_worker.join();
        models_running = true;
        models_done = false;
        models_result = ModelsResult{};
        model_menu_open = false;
        models_fingerprint = fingerprint;
        models_start = steady_now();
        logger->event("models_list",
                      {{"status", "start"},
                       {"base_url", Logger::redact_url(config.base_url)},
                       {"providers", std::to_string(targets.size())}});
        const AppConfig snapshot = config; // thread boundary (like start_turn)
        // The worker computes only the result and sets the flag: `logger` and
        // `screen` are main-thread state (/log reassigns logger), so the
        // completion events and the menu render happen on the main thread.
        models_worker = std::thread([&, snapshot, targets] {
            ModelsResult r;
            for (const auto& [name, creds] : targets) {
                ModelsResult::Part part;
                part.name = name;
                // Bounded wait: a hung /models must not stall the TUI for
                // the full chat timeout budget (design 007 section 6).
                ProviderConfig pc = snapshot.to_provider();
                pc.base_url = creds.first;
                pc.api_key = creds.second;
                pc.connect_timeout_seconds = 5;
                pc.total_timeout_seconds =
                    pc.total_timeout_seconds < 15 ? pc.total_timeout_seconds : 15;
                try {
                    // One provider instance per target: the lazy CURL handle
                    // is NOT thread-safe, and sequential GETs keep the
                    // failure isolation simple (design 012 §5.3).
                    OpenAiProvider provider(std::move(pc));
                    // v4.2: one GET carries both the id list and the optional
                    // capability metadata (OpenRouter-style gateways).
                    part.caps = provider.list_model_capabilities();
                    for (const auto& cap : part.caps)
                        part.ids.push_back(cap.id);
                    part.ok = true;
                } catch (const ProviderError& e) {
                    // parse_models_meta_json shares parse_models_json's fail-
                    // closed root/data guards, so a Protocol error here would
                    // fail the id-only path identically -- no fallback needed.
                    part.error = e.what();
                    part.http_status = e.http_status();
                    part.network_error = e.kind() == ProviderErrorKind::Network;
                } catch (const std::exception& e) {
                    part.error = e.what();
                }
                r.parts.push_back(std::move(part));
            }
            // Legacy fields mirror the ACTIVE target so the existing folds
            // (menu fallback, auth verdict) keep single-profile semantics.
            for (const auto& part : r.parts) {
                if (part.name != snapshot.active_provider)
                    continue;
                r.ok = part.ok;
                r.ids = part.ids;
                r.caps = part.caps;
                r.error = part.error;
                r.http_status = part.http_status;
                r.network_error = part.network_error;
                break;
            }
            models_result = std::move(r);
            models_done = true;
        });
    };

    // Called after /provider or /login mutates config: auto-discover once
    // at least one target has both halves known (the user's stated request).
    auto maybe_autofetch_models = [&] {
        bool any_ready = !config.base_url.empty() && !config.api_key.empty();
        for (const auto& profile : config.providers)
            if (!profile.base_url.empty() && !profile.api_key.empty())
                any_ready = true;
        if (any_ready) {
            screen.commit("[info] listing models from provider...");
            start_models_fetch(false);
        }
    };

    // v5 (design 011): show a workspace file to the human without
    // involving the model. Markdown renders; everything else shows verbatim.
    // Read-only, bounded to the session workspace -- the same rule the file
    // tools enforce (ADR 007). Shared by /preview and the sidebar `p` key.
    auto preview_workspace_file = [&](const std::string& path_raw) {
        std::filesystem::path resolved;
        std::string bounds_error;
        const std::string root = config.workspace.empty()
                                     ? std::filesystem::current_path().string()
                                     : std::filesystem::weakly_canonical(config.workspace).string();
        if (!resolve_within_workspace(root, path_raw, &resolved, &bounds_error)) {
            screen.commit("[error] preview " + sanitize_display(path_raw) + ": " +
                          sanitize_display(bounds_error));
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(resolved, ec)) {
            screen.commit("[error] preview: not a regular file: " + sanitize_display(path_raw));
            return;
        }
        std::ifstream stream(resolved, std::ios::binary);
        if (!stream) {
            screen.commit("[error] preview: cannot open: " + sanitize_display(path_raw));
            return;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string body = buffer.str();
        if (body.find('\0') != std::string::npos) {
            screen.commit("[error] preview: binary file: " + sanitize_display(path_raw));
            return;
        }
        std::string lower = path_raw;
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool markdown =
            lower.size() >= 3 &&
            (lower.compare(lower.size() - 3, 3, ".md") == 0 ||
             (lower.size() >= 9 && lower.compare(lower.size() - 9, 9, ".markdown") == 0));
        screen.commit("[preview] " + sanitize_display(path_raw));
        if (markdown) {
            std::string pline;
            std::istringstream pr(md::render(body, cols_now()));
            while (std::getline(pr, pline))
                screen.commit(pline.empty() ? std::string(" ") : pline);
        } else {
            // v5 (design 011 5.6): recognized code files highlight; colors
            // gate like every other styled path.
            const auto lang = hl::language_for_path(path_raw);
            const bool lit = !lang.empty() && colors_enabled();
            std::string pline;
            std::istringstream pr(body);
            hl::State hstate;
            std::size_t shown = 0;
            while (std::getline(pr, pline) && shown < 500) {
                const auto safe = sanitize_display(pline);
                if (lit)
                    screen.commit(hl::highlight_line(safe, lang, hstate));
                else
                    screen.commit("  " + safe);
                ++shown;
            }
            if (!pr.eof())
                screen.commit("  ... truncated (file longer than 500 lines)");
        }
    };
    // v6 (design 012 §5.2): one selection path for /model and the numbered
    // menu. Cross-profile picks activate the profile first (atomic
    // url+key+model switch), same-profile picks keep the legacy behavior
    // exactly. env overrides stay in effect until the vars are unset and
    // the process restarts -- say so on a profile pick.
    auto select_model = [&](const std::string& provider, const std::string& id,
                            const char* source) {
        const bool switch_profile = !provider.empty() && provider != config.active_provider;
        if (switch_profile) {
            if (!config.activate_provider(provider)) {
                screen.commit("[error] unknown provider '" + provider + "'");
                return;
            }
            switch_active_profile();
            logger->event("provider_activate", {{"provider", provider}, {"source", source}});
        }
        config.model = id;
        config.sync_active_provider();
        session.meta.model = config.model;
        session.meta.provider =
            config.active_provider.empty()
                ? std::optional<std::string>()
                : std::optional<std::string>(config.active_provider);
        reset_usage_readings(); // v4: window/counts belong to the old model
        if (source == std::string_view("menu"))
            save_and_report(); // menu keeps the legacy quiet save
        else
            save_and_report(true);
        logger->event("model_select",
                      {{"model", config.model},
                       {"source", source},
                       {"provider",
                        config.active_provider.empty() ? "(unnamed)" : config.active_provider}});
        screen.commit("model=" + (config.active_provider.empty()
                                      ? config.model
                                      : config.active_provider + "/" + config.model));
        if (switch_profile && config.env_override_active)
            screen.commit("[warn] MINI_AGENT_* env overrides are active: the profile is "
                          "saved, but this process keeps the env endpoint/model until "
                          "the vars are unset and mini-agent restarts");
    };
    auto handle_slash = [&](std::string_view line) -> bool {
        auto arg_of = [](std::string_view text) -> std::string_view {
            const auto space = text.find(' ');
            return space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
        };
        if (line == "/help") {
            // Short lines on purpose: single-row commands stay intact even
            // on narrow terminals (wrap breaks mid-token at width limit).
            screen.commit("commands: /provider <url> · /login [key] · /model <name>");
            screen.commit("          /provider list|add <name> [url]|rm <name> (profiles)");
            screen.commit("          /login <name> [key] · /model <provider>/<name>");
            screen.commit("          /tools (tool registry) · /mcp (servers) · /cwd [path]");
            screen.commit("          /approve ask|allowlist|off (tool gate)");
            screen.commit("          /models (list & pick) · /log <path>|off|default");
            screen.commit(
                "          /log info|debug|trace (capture level; /debug on|all|off alias)");
            screen.commit(
                "          /config · /sessions · /session <id|prefix>|new · /sysinfo on|off");
            screen.commit("          /sidebar on|off (tree + changed files)");
            screen.commit("          /preview <path> (show a file; .md renders)");
            screen.commit("          /exit");
            screen.commit("Each of /provider /login /model saves on its own;");
            screen.commit("configure them in any order, any number of sittings.");
            screen.commit("prompt: ?=unconfigured/unverified · ~=checking · >=ok");
            screen.commit("        !=key rejected · x=provider unreachable");
            screen.commit("Enter sends · Enter queues while streaming");
            return true;
        }
        if (line.rfind("/login", 0) == 0) {
            // v6 (design 012 §5.2): a first token naming a configured
            // profile targets that profile's key; anything else is the key
            // itself for the active route (legacy behavior).
            const auto arg = arg_of(line);
            std::string target = config.active_provider;
            std::string_view key = arg;
            if (!arg.empty()) {
                const auto space = arg.find(' ');
                const auto first = space == std::string_view::npos ? arg : arg.substr(0, space);
                if (config.find_provider(first) != nullptr) {
                    target = std::string(first);
                    key = space == std::string_view::npos ? std::string_view() : arg.substr(space);
                    while (!key.empty() && key.front() == ' ')
                        key.remove_prefix(1);
                }
            }
            const std::string label = target.empty() ? "" : " (" + target + ")";
            if (key.empty()) {
                secret_input = true; // masked composer; next Enter sets key
                // "" = active route; a profile name pins the target even if
                // the active profile changes before Enter lands.
                secret_field = target.empty() ? "api_key" : "api_key:" + target;
                screen.commit("[input] API key" + label +
                              " (hidden); Enter confirms, empty Enter cancels");
            } else {
                if (target == config.active_provider) {
                    config.api_key = std::string(key);
                    auth = AuthProbe{}; // previous verdict belongs to the old key
                    auth_probes[target] = auth;
                    config.sync_active_provider();
                } else {
                    auto* profile = config.find_provider(target);
                    profile->api_key = std::string(key);
                    auth_probes[target] = AuthProbe{};
                }
                save_and_report(true);
                maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/models", 0) == 0) {
            // Listed before /model so the shared prefix resolves here first.
            // v6: any complete profile (or the unnamed route) can be listed;
            // the fetch aggregates every ready target.
            bool any_ready = !config.base_url.empty() && !config.api_key.empty();
            for (const auto& p : config.providers)
                any_ready = any_ready || (!p.base_url.empty() && !p.api_key.empty());
            if (!any_ready) {
                screen.commit("[error] model listing needs base_url + api_key ·" +
                              config_next_hint(config));
            } else if (models_running) {
                screen.commit("[info] already listing models, wait for the list");
            } else {
                screen.commit("[info] listing models from provider...");
                start_models_fetch(true); // force: explicit re-list bypasses cache
            }
            return true;
        }
        if (line.rfind("/model", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const std::string shown =
                    config.active_provider.empty()
                        ? (config.model.empty() ? "(not set)" : config.model)
                        : config.active_provider + "/" + config.model;
                screen.commit("model=" + shown + "; usage /model <name> | <provider>/<name>" +
                              (config.base_url.empty() || config.api_key.empty()
                                   ? std::string()
                                   : " · /models lists what the provider offers"));
            } else {
                // v6 (design 012 §5.2): qualified -> atomic profile switch;
                // bare -> active-profile match, then a globally unique id
                // in the last fetched list; ambiguous names fail closed
                // (never guess an endpoint).
                const std::string ref(arg);
                std::string provider;
                std::string id;
                if (split_qualified_model(ref, &provider, &id) &&
                    config.find_provider(provider) != nullptr) {
                    select_model(provider, id, "slash");
                    return true;
                }
                std::vector<std::string> owners;
                for (std::size_t i = 0; i < model_choice_targets.size(); ++i) {
                    const auto& target = model_choice_targets[i];
                    if (target.second == ref &&
                        std::find(owners.begin(), owners.end(), target.first) == owners.end())
                        owners.push_back(target.first);
                }
                if (owners.size() > 1) {
                    std::string names;
                    for (const auto& owner : owners)
                        names += (names.empty() ? "" : ", ") +
                                 (owner.empty() ? "(unnamed)" : owner);
                    screen.commit("[error] model '" + ref +
                                  "' exists on several providers: " + names);
                    screen.commit("        pick one with /model <provider>/" + ref);
                } else if (owners.size() == 1) {
                    select_model(owners.front(), ref, "slash");
                } else {
                    // Not in the fetched list: literal set on the active
                    // route (legacy behavior -- the id may simply not be
                    // listed).
                    select_model("", ref, "slash");
                }
            }
            return true;
        }
        if (line.rfind("/provider", 0) == 0) {
            const auto arg = arg_of(line);
            auto show_current = [&] {
                // Unnamed state only: with an active profile the "<- active"
                // table row already carries the same values (/config for the
                // full effective view).
                if (!config.active_provider.empty())
                    return;
                screen.commit(
                    "base_url=" + (config.base_url.empty() ? "(not set)" : config.base_url) +
                    " · model=" + (config.model.empty() ? "(not set)" : config.model) +
                    " (unnamed)");
            };
            if (arg.empty() || arg == "list") {
                // v6 (design 012 §5.2): all profiles, keys masked, active
                // marked; unnamed top-level values shown only when no
                // profile is active (the active row already repeats them).
                if (config.providers.empty()) {
                    screen.commit("providers: none · usage /provider add <name> [url]");
                } else {
                    screen.commit("providers (" + std::to_string(config.providers.size()) + "):");
                    for (const auto& p : config.providers)
                        screen.commit("  " + p.name + " · " +
                                      (p.base_url.empty() ? "(url not set)" : p.base_url) +
                                      " · key=" + (p.api_key.empty() ? "(not set)" : "***set***") +
                                      (p.name == config.active_provider ? "  <- active" : ""));
                }
                show_current();
                screen.commit("usage /provider <url> · /provider add <name> [url] · "
                              "/provider rm <name>");
            } else if (arg.rfind("add", 0) == 0 && (arg.size() == 3 || arg[3] == ' ')) {
                auto rest = arg.substr(3);
                while (!rest.empty() && rest.front() == ' ')
                    rest.remove_prefix(1);
                const auto space = rest.find(' ');
                const std::string name(rest.substr(0, space));
                auto url = space == std::string_view::npos ? std::string_view()
                                                           : rest.substr(space + 1);
                while (!url.empty() && url.front() == ' ')
                    url.remove_prefix(1);
                ProviderProfile probe;
                probe.name = name;
                probe.base_url = std::string(url);
                std::string err;
                if (!probe.valid(&err)) {
                    screen.commit("[error] " + err);
                    return true;
                }
                if (auto* existing = config.find_provider(name)) {
                    // Upsert (same rule as MCP): URL replaces when given;
                    // key/model/metadata survive an add without a URL.
                    if (!url.empty()) {
                        existing->base_url = std::string(url);
                        if (name == config.active_provider) {
                            config.base_url = existing->base_url;
                            auth = AuthProbe{};
                            auth_probes[name] = auth;
                        } else {
                            auth_probes.erase(name);
                        }
                    }
                    screen.commit("[ok] provider '" + name + "' updated" +
                                  (url.empty() ? "" : " · " + std::string(url)));
                } else {
                    config.providers.push_back(std::move(probe));
                    screen.commit("[ok] provider '" + name + "' added" +
                                  (url.empty() ? "" : " · " + std::string(url)));
                }
                const auto* added = config.find_provider(name);
                if (added->base_url.empty())
                    screen.commit("     next: /provider add " + name +
                                  " <url> · then /login " + name);
                else if (added->api_key.empty())
                    screen.commit("     next: /login " + name + " (key stays hidden)");
                logger->event("provider_add", {{"provider", name}});
                save_and_report(true);
                maybe_autofetch_models();
            } else if (arg.rfind("rm", 0) == 0 && (arg.size() == 2 || arg[2] == ' ')) {
                auto rest = arg.substr(2);
                while (!rest.empty() && rest.front() == ' ')
                    rest.remove_prefix(1);
                const std::string name(rest);
                auto it = std::find_if(config.providers.begin(), config.providers.end(),
                                       [&](const ProviderProfile& p) { return p.name == name; });
                if (name.empty() || it == config.providers.end()) {
                    screen.commit("[error] unknown provider '" + name +
                                  "' · /provider lists the names");
                    return true;
                }
                const bool was_active = name == config.active_provider;
                config.providers.erase(it);
                auth_probes.erase(name);
                if (was_active) {
                    // Keep the top-level values: removing the active
                    // profile must not leave the next turn unconfigured.
                    config.activate_provider("");
                    switch_active_profile();
                    screen.commit("[info] active profile removed; keeping the current "
                                  "url/key/model as unnamed values");
                }
                logger->event("provider_rm", {{"provider", name}});
                save_and_report(true);
                screen.commit("[ok] provider '" + name + "' removed");
            } else {
                config.base_url = std::string(arg);
                auth = AuthProbe{}; // a different endpoint cannot inherit it
                auth_probes[config.active_provider] = auth;
                config.sync_active_provider();
                save_and_report(true);
                maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/log", 0) == 0) {
            // v4.8 (design 010 §19): /log owns both the metadata-log path
            // (pre-v4.8 behavior, unchanged) and the traffic-capture
            // level ladder: info (default, metadata only) < debug (raw
            // chat dumps) < trace (+ truncated auxiliary traffic).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(logger->enabled() ? "logging on: " + config.log_file
                                                : "logging off; usage /log <path> or /log off");
                screen.commit("level=" + config.log_level +
                              " (session-only) · usage /log info|debug|trace");
            } else if (arg == "info" || arg == "debug" || arg == "trace") {
                // v4.8.1: the level is session-scoped -- set in memory,
                // never saved (persistence would resurrect a debug switch
                // across sessions; the maintainer rule for this knob).
                config.log_level = std::string(arg);
                if (config.log_level == "info") {
                    screen.commit("[ok] log level info (this session) · metadata log only, "
                                  "no raw dumps");
                } else {
                    screen.commit("[ok] log level " + config.log_level + " (this session)" +
                                  (config.log_level == "debug"
                                       ? " · raw chat traffic"
                                       : " · chat + truncated aux traffic") +
                                  " -> " + config.to_provider().debug_log_path);
                }
            } else if (arg == "off") {
                config.log_file = "off"; // sentinel: explicit disable
                logger = make_logger();
                save_and_report();
                screen.commit("[ok] logging disabled");
            } else if (arg == "default") {
                config.log_file.clear(); // empty -> /tmp/mini-agent-<uid>.log
                logger = make_logger();
                save_and_report();
                const auto shown = Logger::resolve_path(config);
                screen.commit("[ok] logging back to default: " +
                              (shown ? *shown : std::string("(none)")));
            } else {
                config.log_file = std::string(arg);
                logger = make_logger();
                save_and_report();
                if (logger->enabled()) {
                    screen.commit("[ok] logging to: " + config.log_file);
                } else {
                    screen.commit("[warn] log path not writable, logging stays off");
                }
            }
            return true;
        }
        if (line == "/config") {
            screen.commit("base_url=" + (config.base_url.empty() ? "(not set)" : config.base_url));
            screen.commit("model=" + (config.model.empty() ? "(not set)" : config.model));
            screen.commit("api_key=" +
                          std::string(config.api_key.empty() ? "(not set)" : "***set***"));
            screen.commit("login=" + std::string(auth_log_token(auth)));
            // v6 (design 012): profile table size + active selection.
            screen.commit("providers=" + std::to_string(config.providers.size()) + " active=" +
                          (config.active_provider.empty() ? "(unnamed)" : config.active_provider) +
                          (config.env_override_active ? " · env override in effect" : ""));
            screen.commit("log_file=" + (config.log_file.empty() ? "(off)" : config.log_file));
            screen.commit(std::string("log_level=") + config.log_level +
                          " (session-only, not saved)");
            // v4 fields (design 010).
            screen.commit("context_window=" +
                          (config.context_window > 0
                               ? format_token_count(config.context_window) + " (" +
                                     std::to_string(config.context_window) + ")"
                               : std::string("(not set)")));
            screen.commit("reasoning_effort=" + (config.reasoning_effort.empty()
                                                     ? std::string("(not set)")
                                                     : config.reasoning_effort));
            screen.commit("config file: " + default_config_path().string() +
                          " (env overrides file)");
            return true;
        }
        if (line.rfind("/sidebar", 0) == 0) {
            // v4.13: right sidebar with the workspace tree + changed
            // files. Session-only display state (same rule as /sysinfo).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(std::string("sidebar=") + (show_sidebar ? "on" : "off") +
                              " · usage /sidebar on|off");
            } else if (arg == "on") {
                show_sidebar = true;
                start_sidebar();
                screen.commit("[ok] sidebar on (this session) · tree + git changes");
            } else if (arg == "off") {
                show_sidebar = false;
                sidebar_focus = false; // v4.14: no focus survives the panel
                screen.commit("[ok] sidebar off (this session)");
            } else {
                screen.commit("[error] /sidebar expects on|off");
            }
            return true;
        }
        if (line.rfind("/sysinfo", 0) == 0) {
            // v4.12: bottom system-info row toggle (user@host · cwd ·
            // clock). Session-only like the log level -- display state,
            // not a durable preference.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(std::string("sysinfo=") + (show_sysinfo ? "on" : "off") +
                              " · usage /sysinfo on|off");
            } else if (arg == "on") {
                show_sysinfo = true;
                screen.commit("[ok] system info row on (session-only)");
            } else if (arg == "off") {
                show_sysinfo = false;
                screen.commit("[ok] system info row off (session-only)");
            } else {
                screen.commit("[error] /sysinfo expects on|off");
            }
            return true;
        }
        if (line == "/sessions") {
            std::string err;
            const auto ids = store.list(&err);
            screen.commit(ids.empty() ? "sessions: (empty)" : "sessions:");
            for (const auto& id : ids)
                screen.commit("  " + id + (id == session.meta.id ? "  <- current" : ""));
            screen.commit("switch in place: /session <id or prefix> · /session new");
            return true;
        }
        if (line.rfind("/session", 0) == 0 && line != "/sessions") {
            // v4.9: switch or fork the live session without exiting. Same
            // idle-gated swap discipline as /cwd (worker join, registry
            // rebuild); the old session is saved and unlocked, the target
            // is loaded and locked. A failed lock/load rolls back.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("session=" + session.meta.id +
                              " · usage /session <id or prefix> | /session new");
                return true;
            }
            if (streaming || turn_done.load()) {
                // The turn-completion handler writes session.chat after
                // joining; swapping mid-fold would land the old turn in
                // the new session. Require idle (Ctrl-C interrupts).
                screen.commit("[info] session switch needs an idle turn · wait or Ctrl-C");
                return true;
            }
            std::string target_id = std::string(arg);
            const bool fresh = target_id == "new";
            if (fresh) {
                target_id = mini_agent::generate_session_id();
            } else if (!mini_agent::session_id_is_safe(target_id)) {
                screen.commit("[error] unsafe session id");
                return true;
            } else {
                // v4.13: prefix match (maintainer request). The typed word
                // resolves against stored ids; one hit replaces it, several
                // list candidates, none falls through so `load` reports the
                // usual "no such session" error.
                session_ids_stale = true;
                refresh_session_ids();
                const auto m = mini_agent::match_session_ref(target_id, session_ids);
                if (m.exact) {
                    target_id = m.id;
                } else if (!m.candidates.empty()) {
                    screen.commit("session " + target_id + " is ambiguous:");
                    for (const auto& id : m.candidates)
                        screen.commit("  " + id + (id == session.meta.id ? "  <- current" : ""));
                    screen.commit("type more characters, or Tab to cycle /session <id>");
                    return true;
                }
            }
            if (target_id == session.meta.id) {
                screen.commit("[info] already on session " + target_id);
                return true;
            }
            std::string err;
            // Persist the outgoing session first; a save failure is a warn
            // only (the switch proceeds; the old file keeps last-turn state).
            // v4.15: a temp session has no file and never gets one, so the
            // pre-switch save is skipped (its chat is dropped on switch-out).
            if (!temp_mode && !store.save(session, &err)) {
                screen.commit("[warn] save before switch failed: " + err);
            }
            Session next;
            std::optional<std::string> next_workspace;
            if (fresh) {
                next.meta.id = target_id;
                next.meta.model = config.model;
                next_workspace = config.workspace.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>(config.workspace);
                next.meta.workspace = next_workspace;
                next.meta.provider =
                    config.active_provider.empty()
                        ? std::optional<std::string>()
                        : std::optional<std::string>(config.active_provider);
                mini_agent::inject_system_prompt(next.chat, config.workspace, config.model,
                                                 config.system_prompt);
            } else {
                auto loaded = store.load(target_id, &err);
                if (!loaded) {
                    screen.commit("[error] load failed: " + sanitize_display(err));
                    return true;
                }
                next = std::move(*loaded);
            }
            if (!store.try_lock(target_id, &err)) {
                screen.commit("[error] session busy: " + sanitize_display(err));
                return true;
            }
            store.unlock(session.meta.id);
            session = std::move(next);
            active_session_id = session.meta.id;
            // v4.15: `/session new` from a temp session stays temp (the new
            // chat is equally ephemeral); resuming a saved id ends temp mode
            // -- from here on the target behaves like a normal session.
            temp_mode = temp_mode && fresh;
            // v3 workspace precedence on switch (same as resume in cmd_run):
            // the session's own stamp rebinds config; else keep current.
            if (session.meta.workspace && !session.meta.workspace->empty()) {
                config.workspace = *session.meta.workspace;
                session.chat.update_system_workspace(config.workspace);
                std::string note;
                tool_registry = build_tool_registry(config, &note);
            }
            config_snapshot = config; // worker turn snapshot refresh (same as /cwd)
            reset_usage_readings();   // usage belongs to the new session
            // Re-seed ↑/↓ history from the new chat (same rule as launch).
            history.clear();
            for (const auto& m : session.chat.messages())
                if (m.role == Role::User && !m.content.empty() && m.content.front() != '/')
                    history.push_back(m.content);
            hist_pos = history.size();
            history_draft.clear();
            logger->event("session_switch", {{"id", session.meta.id},
                                             {"fresh", fresh ? "new" : "resume"},
                                             {"temp", temp_mode ? "on" : "off"}});
            screen.commit(std::string("[ok] ") + (fresh ? "new" : "resumed") +
                          (temp_mode ? " temp" : "") + " session " + session.meta.id + " · " +
                          std::to_string(session.chat.messages().size()) + " message(s)");
            return true;
        }
        if (line.rfind("/context", 0) == 0) {
            // v4 (design 010): model context window for the status-bar
            // ctx% reading. Config is authoritative -- no probing.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const auto inputs = context_reading();
                std::string shown = config.context_window > 0
                                        ? format_token_count(config.context_window) + " (" +
                                              std::to_string(config.context_window) + ")"
                                        : std::string("(not set)");
                // v4.7: when unset, surface the catalog reading (the same
                // value the status bar silently uses) so the user can see
                // and confirm it before it becomes config-authoritative.
                if (config.context_window <= 0 && inputs.context_window > 0)
                    shown += " · catalog: " + format_token_count(inputs.context_window) + " (" +
                             std::to_string(inputs.context_window) + ") · set it: /context " +
                             std::to_string(inputs.context_window);
                screen.commit("context_window=" + shown +
                              "; usage /context <tokens> | /context probe | /context off");
                screen.commit("ctx reading: " +
                              (inputs.context_tokens > 0
                                   ? (inputs.context_estimated ? "~" : "") +
                                         format_token_count(inputs.context_tokens) + " tokens (" +
                                         std::to_string(inputs.context_tokens) + ")" +
                                         (inputs.context_window > 0
                                              ? " = " +
                                                    std::to_string(inputs.context_tokens * 100 /
                                                                   inputs.context_window) +
                                                    "% of window"
                                              : std::string())
                                   : std::string("(no data yet)")) +
                              (status_usage_reported ? " · provider-reported" : " · estimated"));
            } else if (arg == "off" || arg == "0") {
                config.context_window = 0;
                config.sync_active_provider(); // v6: per-profile metadata
                save_and_report();
                screen.commit("[ok] context window cleared · status shows tokens, no %");
            } else if (arg == "probe") {
                // v4.3 (design 010 §13): explicit, user-triggered cheap
                // probe -- one oversized-max_tokens request whose rejection
                // carries the gateway's authoritative numbers. Never
                // automatic: it costs a request and the message text is
                // not an API contract, so the maintainer decides when.
                if (probe_running) {
                    screen.commit("[info] probe already in flight, wait for the result");
                    return true;
                }
                if (config.base_url.empty() || config.api_key.empty() || config.model.empty()) {
                    screen.commit("[error] probe needs base_url + api_key + model ·" +
                                  config_next_hint(config));
                    return true;
                }
                if (probe_worker.joinable())
                    probe_worker.join();
                probe_running = true;
                probe_done = false;
                probe_result = ProbeResult{};
                logger->event("ctx_probe", {{"status", "start"},
                                            {"base_url", Logger::redact_url(config.base_url)},
                                            {"model", config.model}});
                const AppConfig snapshot = config; // thread boundary
                probe_worker = std::thread([&, snapshot] {
                    ProbeResult r;
                    try {
                        ProviderConfig pc = snapshot.to_provider();
                        pc.total_timeout_seconds =
                            pc.total_timeout_seconds < 30 ? pc.total_timeout_seconds : 30;
                        OpenAiProvider provider(pc);
                        r.outcome = provider.probe_context_window();
                    } catch (const ProviderError& e) {
                        r.error = e.what();
                    } catch (const std::exception& e) {
                        r.error = e.what();
                    }
                    probe_result = std::move(r);
                    probe_done = true;
                });
                screen.commit("[info] probing context window (one oversized request)...");
            } else {
                const long long parsed_window = std::strtoll(std::string(arg).c_str(), nullptr, 10);
                if (parsed_window <= 0) {
                    screen.commit("[error] /context expects a positive token count");
                    return true;
                }
                config.context_window = parsed_window;
                config.sync_active_provider(); // v6: per-profile metadata
                save_and_report();
            }
            return true;
        }
        if (line.rfind("/reasoning", 0) == 0) {
            // v4 (design 010): reasoning_effort passthrough. Verbatim
            // value; the server is the arbiter (400 -> hint, no auto-drop:
            // config is authoritative and silent removal would lie about
            // what was sent).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("reasoning_effort=" +
                              (config.reasoning_effort.empty() ? std::string("(not set)")
                                                               : config.reasoning_effort) +
                              "; usage /reasoning <level> or /reasoning off");
                // v4.2 (§11): cached capabilities for the current model ->
                // numbered picker over the gateway's supported levels (the
                // same interaction as /models). A literal value still works
                // for models without advertised metadata.
                const auto caps = caps_for_model();
                if (!caps.reasoning_efforts.empty()) {
                    reasoning_choices = caps.reasoning_efforts;
                    reasoning_menu_open = true;
                    for (const auto& row : reasoning_menu_rows(
                             reasoning_choices, config.reasoning_effort,
                             caps.reasoning_default_effort, caps.reasoning_mandatory))
                        screen.commit(row);
                } else if (caps.supports_reasoning) {
                    screen.commit("[info] gateway marks this model as reasoning-capable "
                                  "but lists no levels; set one explicitly");
                }
            } else if (arg == "off") {
                config.reasoning_effort.clear();
                config.sync_active_provider(); // v6: per-profile metadata
                save_and_report();
                screen.commit("[ok] reasoning effort cleared · requests omit the field");
            } else {
                config.reasoning_effort = std::string(arg);
                config.sync_active_provider(); // v6: per-profile metadata
                save_and_report();
            }
            return true;
        }
        if (line.rfind("/debug", 0) == 0) {
            // v4.8 (design 010 §19): /debug is now an alias of the /log
            // level ladder -- on == /log debug, all == /log trace, off ==
            // /log info. Bodies contain conversation content (the point
            // of the level) while the api_key stays in the Authorization
            // header only.
            const auto arg = arg_of(line);
            auto set_level = [&](std::string_view level, std::string_view ok_text) {
                config.log_level = level; // session-scoped (v4.8.1): no save
                if (config.log_level == "info") {
                    screen.commit(std::string(ok_text) + " · metadata log only");
                } else {
                    screen.commit(std::string(ok_text) + " -> " +
                                  config.to_provider().debug_log_path);
                }
            };
            if (arg.empty()) {
                const auto pc = config.to_provider();
                screen.commit(
                    std::string("debug=") + config.log_level +
                    (config.capture_chat() ? " · file: " + pc.debug_log_path : std::string()) +
                    "; usage /debug on|all|off (alias of /log debug|trace|info)");
            } else if (arg == "on") {
                set_level("debug", "[ok] log level debug (chat traffic) · raw chat traffic");
            } else if (arg == "all") {
                // trace: ALSO dump auxiliary traffic (models listing,
                // context probe request/response), truncated.
                set_level("trace", "[ok] log level trace (chat + truncated aux traffic)");
            } else if (arg == "off") {
                set_level("info", "[ok] log level info · no further dumps");
            } else if (arg == "debug" || arg == "trace" || arg == "info") {
                set_level(arg, arg == "info"
                                   ? "[ok] log level info"
                                   : (arg == "debug" ? "[ok] log level debug (chat traffic)"
                                                     : "[ok] log level trace (chat + aux)"));
            } else {
                screen.commit("[error] /debug expects on|all|off|info|debug|trace");
            }
            return true;
        }
        if (line.rfind("/approve", 0) == 0) {
            // v3.1 runtime approval switch: /approve [ask|allowlist|off].
            // Worker must be idle (policy is snapshotted per turn), same
            // join discipline as /cwd and /tools.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("approval=" + approval_policy +
                              " · /approve ask|allowlist|off · 'a' key also "
                              "allows a tool name for this session");
                if (approval_policy == "allowlist") {
                    std::string joined;
                    for (const auto& name : config.tool_allowlist) {
                        if (!joined.empty())
                            joined += ", ";
                        joined += name;
                    }
                    screen.commit("allowlist: " +
                                  (joined.empty() ? std::string("(empty)") : joined));
                }
                return true;
            }
            const std::string want(arg);
            if (want != "ask" && want != "allowlist" && want != "off") {
                screen.commit("[error] /approve expects ask|allowlist|off");
                return true;
            }
            if (worker.joinable())
                worker.join();
            approval_policy = want;
            config.tool_approval = want;
            config_snapshot = config; // worker turn snapshot refresh (same as /cwd)
            save_and_report(false);
            if (want == "off" && !tool_registry->empty())
                screen.commit("[warn] tool_approval=off: tools run WITHOUT asking"
                              " (no sandbox: run_bash executes at your user's "
                              "privileges; git push stays human-only by tool "
                              "absence, but shell can still reach remotes)");
            else if (want == "allowlist" && config.tool_allowlist.empty())
                screen.commit("[info] allowlist mode with an EMPTY list: every tool "
                              "still asks; add names via tool_allowlist in config.json");
            else
                screen.commit("[ok] approval=" + approval_policy + " saved");
            logger->event("approval_switch", {{"policy", approval_policy}});
            return true;
        }
        if (line.rfind("/cwd", 0) == 0) {
            // v3 (design 009 5.4): query or rebind the session workspace.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("workspace: " +
                              sanitize_display(config.workspace.empty()
                                                   ? std::filesystem::current_path().string()
                                                   : config.workspace) +
                              " · /cwd <path> to rebind (file+git tools and run_bash)");
                return true;
            }
            const std::filesystem::path target(arg);
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(target, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                screen.commit("[error] not a directory: " + sanitize_display(arg));
                return true;
            }
            if (worker.joinable())
                worker.join(); // registry swap needs the worker idle (008 thread rule)
            config.workspace = canonical.string();
            session.meta.workspace = config.workspace;
            session.chat.update_system_workspace(config.workspace);
            std::string note;
            tool_registry = build_tool_registry(config, &note);
            config_snapshot = config;
            start_sidebar(); // v4.13: tree belongs to the new boundary
            screen.commit("[ok] workspace=" + sanitize_display(config.workspace) +
                          " · tools rebound · " +
                          (config.workspace == std::filesystem::current_path().string()
                               ? "(matches launch cwd)"
                               : "run_bash now starts inside it"));
            return true;
        }
        if (line.rfind("/preview", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("usage: /preview <path> (workspace-bounded; .md renders)");
                return true;
            }
            preview_workspace_file(std::string(arg));
            return true;
        }
        if (line == "/tools") {
            // Rebuild the registry so fresh server configs apply without
            // a restart; old clients die with their transports (RAII).
            if (worker.joinable())
                worker.join();
            config_snapshot = config;
            std::string note;
            tool_registry = build_tool_registry(config, &note);
            std::vector<std::string> rendered;
            for (const auto& definition : tool_registry->list()) {
                auto first_line = definition.description.find('\n');
                rendered.push_back(definition.name + " \u00b7 " + definition.source + " \u00b7 " +
                                   sanitize_display(definition.description.substr(0, first_line)));
            }
            for (const auto& row : tool_registry_rows(rendered))
                screen.commit(row);
            if (!note.empty())
                screen.commit("[info] " + note + " (lazy start on first call)");
            screen.commit("approval=" + approval_policy);
            return true;
        }
        if (line == "/mcp") {
            if (config.mcp_servers.empty()) {
                screen.commit("mcp servers: (none) · add mcp_servers to config.json");
                return true;
            }
            for (const auto& server : config.mcp_servers) {
                screen.commit("  " + server.name + " \u00b7 " + server.transport + "\u2192" +
                              (server.transport == "http" ? Logger::redact_url(server.url)
                                                          : sanitize_display(server.command)));
            }
            screen.commit("restart servers after config edits: /tools · down marks clear per call");
            return true;
        }
        if (line == "/exit")
            return false; // caller exits
        if (!line.empty() && line.front() == '/') {
            screen.commit("[error] unknown command " + std::string(line) + " (/help)");
            return true;
        }
        return false;
    };

    auto hist_up = [&] {
        if (hist_pos == 0)
            return;
        if (hist_pos == history.size())
            history_draft = composer; // leaving the draft slot: keep it
        composer = history[--hist_pos];
        cursor = composer.size();
    };
    auto hist_down = [&] {
        if (hist_pos == history.size())
            return;
        if (++hist_pos == history.size()) {
            composer = history_draft;
            history_draft.clear();
        } else {
            composer = history[hist_pos];
        }
        cursor = composer.size();
    };
    // Editing a recalled line parks the edit in the draft slot without
    // moving hist_pos (bash semantics): Down returns to it later.
    auto mark_edited = [&] {
        if (hist_pos != history.size())
            history_draft = composer;
    };

    for (;;) {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(options.frame_ms));
        if (ready > 0 && (pfd.revents & POLLIN)) {
            char buf[256];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                break; // EOF: Ctrl-D or closed stdin
            }
            for (ssize_t i = 0; i < n; ++i) {
                const unsigned char c = static_cast<unsigned char>(buf[i]);
                if (approval_pending()) {
                    // Approval mode owns the keyboard (008 5.4): only
                    // y/n/a/? act; Esc and Ctrl-C deny the tool AND keep
                    // the existing interrupt escalation (abort_requested),
                    // so the turn stops cleanly instead of hanging.
                    if (c == 'y' || c == 'Y' || c == 'n' || c == 'N' || c == 'a' || c == 'A' ||
                        c == '?') {
                        answer_approval(c);
                        continue;
                    }
                    if (c == 0x03 || c == 0x1b) {
                        answer_approval('n');
                        abort_requested = true;
                        continue;
                    }
                    continue; // held: swallowed without touching composer
                }
                if (c == 0x03) { // Ctrl-C
                    if (streaming) {
                        if (abort_requested.load()) {
                            // already interrupted, waiting on worker: quit
                            goto done;
                        }
                        abort_requested = true; // interrupt; redirect per 5.6
                    } else {
                        const auto t = steady_now();
                        if (t - last_ctrl_c < std::chrono::seconds(2)) {
                            goto done;
                        }
                        last_ctrl_c = t;
                        composer.clear();
                        cursor = 0;
                        screen.commit("[hint] press Ctrl-C again to quit, or type a new prompt");
                    }
                } else if (c == 0x04) { // Ctrl-D
                    goto done;
                } else if (c == 0x0C) { // Ctrl-L
                    screen.clear_live();
                } else if (c == 0x06 && show_sidebar) { // Ctrl-F: sidebar focus (v4.14)
                    sidebar_focus = !sidebar_focus;
                    if (sidebar_focus) {
                        sidebar_sel = 0;
                        // land on the first selectable row
                        for (std::size_t i = 0; i < sidebar_result.paths.size(); ++i) {
                            if (!sidebar_result.paths[i].empty()) {
                                sidebar_sel = i;
                                break;
                            }
                        }
                        // v5: make the preview key discoverable on entry.
                        screen.commit("[hint] sidebar focus: Up/Down walk · p=preview · "
                                      "Enter=insert path · Esc=leave");
                    }

                } else if (c == 0x1B) { // ESC: edit sequences or interrupt
                    const ssize_t esc = i;
                    ssize_t j = i + 1;
                    if (j < n && (buf[j] == '[' || buf[j] == 'O')) {
                        // Full CSI/SS3 sequence (fixes plain-text '[' +
                        // letter leaking into the composer when arrows were
                        // typed). Consume and dispatch it.
                        ++j;
                        while (j < n && !(buf[j] >= 0x40 && buf[j] <= 0x7E))
                            ++j;
                        if (j >= n) {
                            i = n; // partial sequence at chunk edge: swallow
                            continue;
                        }
                        const unsigned char fin = static_cast<unsigned char>(buf[j]);
                        i = j;
                        if (!secret_input) { // masked key entry: no cursor UI
                            // v4.14: while the sidebar owns focus, Up/Down
                            // are MODAL: consumed here even when the scan
                            // has no rows yet (falling through to history
                            // recall could auto-send a stale message).
                            const bool nav = sidebar_focus;
                            auto move_sel = [&](int delta) {
                                const auto& paths = sidebar_result.paths;
                                if (paths.empty())
                                    return;
                                std::size_t cur = sidebar_sel;
                                for (std::size_t step = 0; step < paths.size(); ++step) {
                                    cur = (cur + paths.size() + static_cast<std::size_t>(delta)) %
                                          paths.size();
                                    if (!paths[cur].empty()) {
                                        sidebar_sel = cur;
                                        return;
                                    }
                                }
                            };
                            switch (fin) {
                            case 'A':
                                if (nav)
                                    move_sel(-1);
                                else
                                    hist_up();
                                break;
                            case 'B':
                                if (nav)
                                    move_sel(+1);
                                else
                                    hist_down();
                                break;
                            case 'C':
                                cursor = utf8_next(composer, cursor);
                                break;
                            case 'D':
                                cursor = utf8_prev(composer, cursor);
                                break;
                            case 'H':
                                cursor = 0;
                                break;
                            case 'F':
                                cursor = composer.size();
                                break;
                            case '~': {
                                const std::string_view params(
                                    buf + esc + 2, static_cast<std::size_t>(j - esc - 2));
                                if (params == "3") {
                                    if (utf8_erase_at(composer, cursor))
                                        mark_edited();
                                } else if (params == "1") {
                                    cursor = 0;
                                } else if (params == "4") {
                                    cursor = composer.size();
                                }
                                break;
                            }
                            default:
                                break; // swallow other CSI (paste marks, F-keys)
                            }
                        }
                    } else if (sidebar_focus) {
                        // v4.14: lone ESC leaves the sidebar focus first
                        // (before the interrupt chain -- focus is modal UI,
                        // same precedence as the menus below).
                        sidebar_focus = false;
                    } else if (streaming) {
                        // Lone ESC interrupts the in-flight request (design 5.6
                        // rule 5 "esc-to-interrupt", 2026-11-02 maintainer
                        // request): same effect as the first Ctrl-C, without
                        // the double-press-to-quit escalation.
                        abort_requested = true;
                    } else if (reasoning_menu_open) {
                        reasoning_menu_open = false;
                        reasoning_choices.clear();
                        screen.commit("[cancel] reasoning menu dismissed; /reasoning re-opens");
                    } else if (model_menu_open) {
                        // Lone ESC while the model list is up dismisses it
                        // (nothing else to interrupt); the fetched list stays
                        // in the scrollback and /model <name> still works.
                        model_menu_open = false;
                        screen.commit("[cancel] model menu dismissed; /models re-lists");
                    }
                } else if (c == '\r' || c == '\n') {
                    if (sidebar_focus) {
                        // Modal Enter: insert the highlighted path, or just
                        // leave focus when the scan has nothing selectable
                        // yet -- either way the composer never sees this
                        // Enter (no accidental send of a half-built line).
                        if (sidebar_sel < sidebar_result.paths.size() &&
                            !sidebar_result.paths[sidebar_sel].empty()) {
                            // v4.14: pick the highlighted file/dir -- insert the
                            // workspace-relative path at the caret, leave focus
                            // so the composer keys behave normally again.
                            const std::string& rel = sidebar_result.paths[sidebar_sel];
                            utf8_insert(composer, cursor, rel); // advances cursor
                            mark_edited();
                            screen.commit("[sidebar] inserted: " + sanitize_display(rel) +
                                          " · edit freely, Enter sends as usual");
                        }
                        sidebar_focus = false;
                    } else if (secret_input) {
                        secret_input = false;
                        if (!composer.empty()) {
                            // v6: `api_key` = active route; `api_key:<name>`
                            // pins a profile chosen at /login <name> time.
                            bool ok = true;
                            if (secret_field == "api_key") {
                                config.api_key = composer;
                                config.sync_active_provider();
                                auth = AuthProbe{}; // stale the moment it changes
                                auth_probes[config.active_provider] = auth;
                            } else if (secret_field.rfind("api_key:", 0) == 0) {
                                const std::string name = secret_field.substr(8);
                                if (auto* profile = config.find_provider(name)) {
                                    profile->api_key = composer;
                                    auth_probes[name] = AuthProbe{};
                                    if (name == config.active_provider)
                                        auth = AuthProbe{};
                                } else {
                                    ok = false;
                                    screen.commit("[error] provider '" + name +
                                                  "' no longer exists; key not saved");
                                }
                            }
                            if (ok) {
                                screen.commit("[ok] api_key updated");
                                save_and_report(true);
                                maybe_autofetch_models();
                            }
                        } else {
                            screen.commit("[cancel] input cancelled");
                        }
                        composer.clear();
                        cursor = 0;
                    } else if (!composer.empty()) {
                        std::string line;
                        line.swap(composer);
                        cursor = 0;
                        // Model menu: a number (or an exact id) selects. Any
                        // other input dismisses the menu and falls through
                        // unchanged, so /model <name> and ordinary questions
                        // keep working while the list is on screen.
                        if (reasoning_menu_open) {
                            reasoning_menu_open = false;
                            // Same numbered-choice helper as the model menu.
                            if (const auto pick = model_choice(reasoning_choices, line)) {
                                config.reasoning_effort = *pick;
                                config.sync_active_provider(); // v6: per-profile
                                save_and_report();
                                logger->event(
                                    "reasoning_select",
                                    {{"level", config.reasoning_effort}, {"source", "menu"}});
                                screen.commit("reasoning_effort=" + config.reasoning_effort);
                                line.clear();
                            }
                        }
                        if (model_menu_open) {
                            model_menu_open = false;
                            if (const auto pick = model_choice(model_choices, line)) {
                                // v6: the label's index resolves the owning
                                // profile; select_model handles the switch.
                                const auto at =
                                    std::find(model_choices.begin(), model_choices.end(), *pick);
                                const std::size_t idx =
                                    static_cast<std::size_t>(at - model_choices.begin());
                                if (idx < model_choice_targets.size()) {
                                    const auto target = model_choice_targets[idx];
                                    select_model(target.first, target.second, "menu");
                                } else {
                                    select_model("", *pick, "menu");
                                }
                                line.clear();
                            }
                        }
                        if (line.empty()) {
                            // menu selection consumed the input
                        } else {
                            if (line.front() != '/') // slash lines: Tab menu recalls those
                                history.push_back(line);
                            hist_pos = history.size();
                            history_draft.clear();
                            if (line == "/exit")
                                goto done;
                            if (streaming) {
                                // Pi-style steering (design 007 section 5.6
                                // v1.x candidate 1): Enter while streaming
                                // queues the input; it is sent after the
                                // current turn commits (or is interrupted).
                                pending.push_back(std::move(line));
                            } else if (!handle_slash(line)) {
                                if (!config.complete()) {
                                    screen.commit("[error] LLM not configured ·" +
                                                  config_next_hint(config));
                                } else {
                                    start_turn(line);
                                }
                            }
                        }
                    }
                } else if (c == '\t') {
                    if (const auto completed = slash_tab_complete(composer, tab_state, session_ids,
                                                                  provider_name_list(),
                                                                  model_choices)) {
                        composer = *completed;
                        cursor = composer.size();
                        mark_edited();
                    }
                } else if (c == 0x7F || c == 0x08) {
                    if (utf8_erase_before(composer, cursor))
                        mark_edited();
                } else if (c >= 0x20) {
                    // v5 (design 011): while the sidebar owns focus, `p`
                    // previews the highlighted file into the transcript
                    // (no model turn, focus stays); any other printable
                    // char goes to the composer as usual.
                    if (sidebar_focus && c == 'p' && sidebar_sel < sidebar_result.paths.size() &&
                        !sidebar_result.paths[sidebar_sel].empty()) {
                        preview_workspace_file(sidebar_result.paths[sidebar_sel]);
                    } else {
                        utf8_insert(composer, cursor, std::string_view(&buf[i], 1));
                        mark_edited();
                    }
                }
            }
        }

        // frame: drain buffer snapshot and render live region (5.6 rule 1)

        // Model-list completion: rendered here on the main thread so the
        // worker never touches screen/logger (both are main-thread state).
        if (models_done.load()) {
            models_done = false;
            models_running = false;
            if (models_worker.joinable())
                models_worker.join();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(steady_now() - models_start)
                    .count();
            // v6 (design 012 §5.3): fold every target's sub-result. The
            // credential probe verdict lands in the per-profile map; menu
            // rows aggregate all successful listings (qualified labels when
            // more than one target answered, bare ids for the legacy single
            // route so the existing `models (N)` contract stays intact).
            const bool aggregate = models_result.parts.size() > 1;
            model_choices.clear();
            model_choice_targets.clear();
            model_caps.clear(); // rebuilt from the ACTIVE target below
            std::vector<std::string> failures;
            bool any_ok = false;
            bool active_probed = false;
            for (auto& part : models_result.parts) {
                std::vector<std::pair<std::string, std::string>> list_fields = {
                    {"provider", part.name.empty() ? "(unnamed)" : part.name},
                    {"elapsed_ms", std::to_string(ms)},
                };
                if (part.ok) {
                    any_ok = true;
                    list_fields.emplace_back("status", "ok");
                    list_fields.emplace_back("count", std::to_string(part.ids.size()));
                    logger->event("models_list", list_fields);
                    for (const auto& id : part.ids) {
                        model_choices.push_back(aggregate && !part.name.empty()
                                                    ? part.name + "/" + id
                                                    : id);
                        model_choice_targets.emplace_back(part.name, id);
                    }
                    if (part.name == config.active_provider)
                        model_caps = std::move(part.caps); // v4.2
                    part.caps.clear();
                } else {
                    list_fields.emplace_back("status", "error");
                    list_fields.emplace_back("count", "0");
                    list_fields.emplace_back("error", part.error);
                    logger->event("models_list", list_fields);
                    failures.push_back((part.name.empty() ? std::string("(unnamed)") : part.name) +
                                       ": " + part.error);
                }
                // The request is also the credential probe: a 401 means
                // re-enter /login, a 200 with an unparseable body still
                // proves the key is good. Per-profile verdict (design 012).
                AuthProbe probe;
                probe.attempted = true;
                probe.fetch_ok = part.ok;
                probe.http_status = part.http_status;
                probe.network_error = part.network_error;
                probe.model_count = part.ids.size();
                auth_probes[part.name] = probe;
                if (part.name == config.active_provider) {
                    auth = probe;
                    active_probed = true;
                }
                std::vector<std::pair<std::string, std::string>> login_fields = {
                    {"status", std::string(auth_log_token(probe))},
                    {"models", std::to_string(probe.model_count)},
                    {"provider", part.name.empty() ? "(unnamed)" : part.name},
                };
                // Only report a status code when one actually arrived: a
                // success path never raised one, and writing `http_status=0`
                // there would read like a bogus response rather than "not
                // applicable".
                if (probe.http_status != 0)
                    login_fields.emplace_back("http_status", std::to_string(probe.http_status));
                logger->event("login", login_fields);
            }
            model_menu_open = !model_choices.empty();
            if (model_menu_open) {
                // `<- current` tags the active combination's label.
                const std::string current_label =
                    aggregate && !config.active_provider.empty()
                        ? config.active_provider + "/" + config.model
                        : config.model;
                for (const auto& row : model_menu_rows(model_choices, current_label))
                    screen.commit(row);
            } else if (!any_ok) {
                screen.commit("[error] model list failed: " + models_result.error);
            } else {
                screen.commit("[info] provider listed no models; use /model <name>");
            }
            // Failures never block the successful listings: summarize them
            // at the menu tail, one row per failed profile.
            for (const auto& failure : failures)
                screen.commit("[warn] model list failed for " + failure);
            if (active_probed)
                screen.commit(auth_status_line(auth));
        }

        // v4.3: window-probe completion (main-thread rendering).
        if (probe_done.load()) {
            probe_done = false;
            probe_running = false;
            if (probe_worker.joinable())
                probe_worker.join();
            if (!probe_result.error.empty()) {
                logger->event("ctx_probe", {{"status", "error"}, {"error", probe_result.error}});
                screen.commit("[error] probe failed: " + probe_result.error);
            } else if (probe_result.outcome.kind == ProbeLimitKind::Window ||
                       probe_result.outcome.kind == ProbeLimitKind::InputCap) {
                // v4.3.1: only a total window or an input cap may become
                // the ctx denominator (the prompt side of the reading).
                const auto kind = probe_result.outcome.kind;
                const long long window = probe_result.outcome.value;
                config.context_window = window;
                config.sync_active_provider(); // v6: per-profile metadata
                save_and_report();
                logger->event("ctx_probe",
                              {{"status", "ok"},
                               {"window", std::to_string(window)},
                               {"kind", kind == ProbeLimitKind::Window ? "window" : "input_cap"}});
                screen.commit("[ok] probed " +
                              std::string(kind == ProbeLimitKind::Window ? "context window: "
                                                                         : "input limit: ") +
                              format_token_count(window) + " (" + std::to_string(window) +
                              " tokens) · saved to config (override: /context <tokens>)");
            } else if (probe_result.outcome.kind == ProbeLimitKind::OutputCap) {
                // The gateway's max_tokens validator fired first: that is
                // the OUTPUT cap, not the window. Show it, save nothing.
                logger->event("ctx_probe", {{"status", "output_cap"},
                                            {"value", std::to_string(probe_result.outcome.value)}});
                screen.commit("[info] gateway reported its OUTPUT cap: " +
                              format_token_count(probe_result.outcome.value) + " (" +
                              std::to_string(probe_result.outcome.value) +
                              " tokens) · not the context window · set the window: "
                              "/context <tokens>");
            } else {
                logger->event("ctx_probe", {{"status", "unknown"}});
                screen.commit("[info] gateway accepted the oversized request without reporting a "
                              "limit (silent clamp) · set the window manually: /context <tokens>");
            }
        }

        // v4.13: sidebar scan completion (main-thread rendering).
        if (sidebar_done.load()) {
            sidebar_done = false;
            sidebar_running = false;
            if (sidebar_worker.joinable())
                sidebar_worker.join();
            if (sidebar_stale) {
                sidebar_stale = false;
                start_sidebar(); // the turn/cwd changed mid-scan: rescan
            }
        }

        if (turn_done.load()) {
            streaming = false;
            if (worker.joinable())
                worker.join();
            drain_tool_events(); // last tool rows commit before the reply
            const auto login_before = auth_log_token(auth);
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(steady_now() - turn_start)
                    .count();
            if (result.ok) {
                // v4: fold this turn's usage into the status-bar readings.
                // prompt side = live context (last round), completion side
                // = session sum. Turns without a usage chunk leave the
                // estimate fallback in play (context_estimated flips).
                if (!result.usage.empty()) {
                    if (result.usage.prompt_tokens > 0) {
                        status_context_tokens = result.usage.prompt_tokens;
                        status_context_estimated = false;
                    }
                    status_completion_tokens += result.usage.completion_tokens;
                    status_usage_reported = true;
                }
                estimate_dirty = true;
                // v4.2.1: the reading reflects the LAST successful turn
                // only (no sticky "on"): fold the fresh evidence wholesale,
                // clearing it when this turn showed none.
                status_reasoning_seen = result.reasoning.seen;
                status_reasoning_actual = result.reasoning.effort;
                logger->event("turn", {{"status", "ok"},
                                       {"elapsed_ms", std::to_string(elapsed_ms)},
                                       {"chars_in", std::to_string(last_prompt_chars)},
                                       {"chars_out", std::to_string(result.reply.size())}});
                screen.clear_live();
                screen.commit("You: " + last_user);
                if (::isatty(STDOUT_FILENO)) {
                    for (auto& para : split_ansi_paragraphs(md::render(result.reply, cols_now()))) {
                        screen.commit(para);
                    }
                } else {
                    screen.commit("AI: " + result.reply);
                }
                std::string err;
                if (temp_mode) {
                    // v4.15: temp sessions never touch the store.
                } else if (!store.save(session, &err)) {
                    screen.commit("[warn] save failed: " + err);
                    logger->event("session_save", {{"status", "error"}, {"error", err}});
                }
                start_sidebar(); // v4.13: the turn may have changed files
            } else if (result.aborted) {
                logger->event("turn",
                              {{"status", "aborted"}, {"elapsed_ms", std::to_string(elapsed_ms)}});
                screen.clear_live();
                screen.commit("[interrupted] You: " + last_user);
            } else {
                logger->event("turn", {{"status", "error"},
                                       {"elapsed_ms", std::to_string(elapsed_ms)},
                                       {"error", result.error}});
                screen.clear_live();
                screen.commit("You: " + last_user);
                screen.commit("[error] " + result.error);
                // v4: a 400 right after we started sending reasoning_effort
                // is most likely the gateway rejecting the field -- name the
                // probable cause and the fix instead of leaving the raw error.
                if (result.http_status == 400 && !config.reasoning_effort.empty())
                    screen.commit("[hint] if this gateway does not support reasoning_effort, "
                                  "clear it with /reasoning off (config value is otherwise "
                                  "kept verbatim)");
                if (tools_active)
                    screen.commit("[note] tools already ran this turn; their side "
                                  "effects were not rolled back");
            }
            // The completion request was itself a credential probe, so fold its
            // outcome into the same verdict the model listing feeds: a working
            // chat must read as connected even on providers that expose no
            // /models endpoint (2026-11-02 report: chat replied, the prompt
            // stayed yellow). Only real evidence counts -- an aborted turn or a
            // local protocol error says nothing about the key and leaves the
            // previous verdict alone.
            if (result.ok) {
                auth.attempted = true;
                auth.fetch_ok = true;
                auth.http_status = 0;
                auth.network_error = false;
            } else if (!result.aborted && (result.http_status != 0 || result.network_error)) {
                auth.attempted = true;
                auth.fetch_ok = false;
                auth.http_status = result.http_status;
                auth.network_error = result.network_error;
            }
            // v6: the verdict belongs to the active profile (the route the
            // turn actually ran on); the map keeps it across switches.
            auth_probes[config.active_provider] = auth;
            // The prompt marker is the user-facing feedback (2026-11-02: the
            // [auth] history row that used to go with this is deliberate noise
            // on every first reply), so a chat-driven verdict change is only
            // recorded in the diagnostics log.
            if (auth_log_token(auth) != login_before) {
                std::vector<std::pair<std::string, std::string>> login_fields = {
                    {"status", std::string(auth_log_token(auth))},
                    {"models", std::to_string(auth.model_count)},
                    {"source", "chat"},
                    {"provider",
                     config.active_provider.empty() ? "(unnamed)" : config.active_provider},
                };
                // Status code only when one actually arrived (same rule as the
                // model-list event: `http_status=0` reads like a bogus reply).
                if (auth.http_status != 0)
                    login_fields.emplace_back("http_status", std::to_string(auth.http_status));
                logger->event("login", login_fields);
            }
            turn_done = false;
            // Drain the steering queue once the turn commits (Pi-style,
            // 5.6 v1.x #1). Skipped while exiting.
            if (!exiting && !pending.empty()) {
                std::string next = std::move(pending.front());
                pending.erase(pending.begin());
                if (next == "/exit") {
                    goto done;
                }
                if (!handle_slash(next)) {
                    if (config.complete()) {
                        start_turn(next);
                    } else {
                        screen.commit("[error] LLM not configured, dropped queued message ·" +
                                      config_next_hint(config));
                    }
                } else {
                    continue; // slash handled, keep rendering idle frame
                }
            }
        } else {
            const auto cols = static_cast<std::size_t>(term_columns());
            drain_tool_events();                 // [tool] rows land in history as they arrive
            std::vector<std::string> extra_rows; // approval block when pending
            const std::string tail = streaming ? bridge.deltas_snapshot() : std::string{};
            // v4.11 turn panel: while streaming, the extra-rows slot shows
            // the process (thinking marker + tool rows incl. the running
            // call). Approval rows keep priority when a decision is pending
            // (the pending call IS the process at that moment).
            if (streaming && !approval_pending()) {
                std::vector<std::string> panel_tools = panel_tool_rows;
                if (const auto running = bridge.running_tool(); !running.empty())
                    panel_tools.push_back(running);
                const bool thinking = reasoning_live.load(std::memory_order_relaxed);
                const auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(steady_now() - turn_start)
                        .count();
                extra_rows = turn_panel_rows(colors_enabled(), thinking, secs, panel_tools, 5);
            }
            std::string composer_display;
            if (secret_input) {
                composer_display.assign(composer.size(), '*');
                if (composer_display.empty())
                    composer_display = "(type key, Enter to confirm)";
            } else if (composer.empty()) {
                // Caret-only cell keeps the reverse-video block visible at
                // an empty prompt; the queue hint trails it.
                composer_display = "\x1b[7m \x1b[27m";
                if (streaming)
                    composer_display += "(streaming… type to queue)";
            } else {
                // Reverse-video block on the codepoint under the caret (or a
                // blank at line end). md::wrap treats the SGR as zero width,
                // so row accounting (5.6 rule 2) is unaffected.
                composer_display = composer.substr(0, cursor);
                composer_display += "\x1b[7m";
                if (cursor < composer.size()) {
                    const std::size_t nxt = utf8_next(composer, cursor);
                    composer_display.append(composer, cursor, nxt - cursor);
                    composer_display += "\x1b[27m";
                    composer_display.append(composer, nxt, std::string::npos);
                } else {
                    composer_display += ' ';
                }
                composer_display += "\x1b[27m";
            }
            // Masked input passes an empty raw composer: no slash hint.
            const std::string raw_for_hint = secret_input ? std::string() : composer;
            // v4.13: while the composer sits in the /session arg tier
            // ("/session …"), keep the id snapshot current so Tab and the
            // hint list see sessions saved by this or another process.
            if (!session_ids_stale && starts_with(raw_for_hint, "/session "))
                session_ids_stale = true;
            if (session_ids_stale)
                refresh_session_ids();
            // Prompt marker = provider connection state, derived purely from
            // (config, last probe, listing-in-flight): an idle tick yields the
            // same text, so render() keeps its zero-work input cache.
            const std::string prompt =
                prompt_marker(connection_state(config, auth, models_running));
            // Status-row colouring (2026-11-04 readability request): the
            // identity prefix dims, state words carry the hue (listing/
            // streaming cyan, idle dim, auto-tools red, AWAITING APPROVAL
            // bold yellow). SGR is zero-width for md::clip, and the clip
            // keeps any active span (it re-emits escapes while cutting).
            const bool sc = colors_enabled();
            // v4.15: the id follows /session switches (like the banner) and
            // carries the temp tag; was options.session_id (launch snapshot).
            std::string status = tint(
                sc, "2",
                "model=" + (config.model.empty() ? std::string("(not set)") : config.model) +
                    " · session=" + active_session_id + (temp_mode ? " (temp)" : std::string()));
            if (models_running) {
                status += tint(sc, "36", " · listing models...");
            } else if (model_menu_open) {
                status += tint(sc, "36", " · pick a model (Esc cancels)");
            }
            if (streaming) {
                const auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(steady_now() - turn_start)
                        .count();
                status +=
                    tint(sc, "36", " · streaming (Ctrl-C/Esc) · " + std::to_string(secs) + "s");
                if (!pending.empty()) {
                    status += tint(sc, "36", " · queued " + std::to_string(pending.size()));
                }
            } else {
                status += tint(sc, "2", " · idle");
            }
            if (approval_policy == "off" && !tool_registry->empty())
                status += tint(sc, "1;31", " · auto-tools"); // persistent danger marker (008 5.4)
            // v4: usage tail (ctx/out/reasoning) before transient states so
            // streaming/approval messages keep their established position.
            status += status_usage_segments(sc, context_reading());
            if (approval_pending()) {
                const ToolCall snapshot = bridge.approval_call();
                const std::string snapshot_preview = bridge.approval_preview();
                status += tint(sc, "1;33", " · AWAITING APPROVAL");
                // v4.17: after 90 s of the 120 s auto-deny budget, show the
                // remaining seconds. The line changes once per second, which
                // naturally busts render()'s input cache (no per-frame
                // repaint otherwise).
                if (const long long waited_s = bridge.approval_waited_s();
                    waited_s >= kApprovalWarnAtS) {
                    const long long left = std::max<long long>(0, kApprovalTimeoutS - waited_s);
                    status += tint(sc, "1;31", " · auto-deny in " + std::to_string(left) + "s");
                }
                extra_rows = approval_rows(snapshot.name, snapshot.arguments_json,
                                           approval_expanded, snapshot_preview);
            }
            // v4.12: bottom system-info row (user@host · cwd · clock).
            // Gathered per frame: the clock is the only fast-changing part
            // and row-level diffing keeps the repaint cost at one row per
            // second. Home abbreviated to ~ like the Pi footer; workspace
            // stamp (session boundary) shown when set, else process cwd.
            std::string sysinfo;
            if (show_sysinfo) {
                std::string user;
                if (const char* u = ::getenv("USER"))
                    user = u;
                else if (const auto* pw = ::getpwuid(::getuid()))
                    user = pw->pw_name;
                else
                    user = "?";
                char hostbuf[256] = {0};
                if (::gethostname(hostbuf, sizeof(hostbuf) - 1) != 0)
                    std::snprintf(hostbuf, sizeof(hostbuf), "?");
                std::string cwd = config.workspace.empty()
                                      ? std::filesystem::current_path().string()
                                      : config.workspace;
                if (const char* home = ::getenv("HOME"); home && *home) {
                    const std::string h = home;
                    if (cwd.size() >= h.size() && cwd.compare(0, h.size(), h) == 0)
                        cwd = "~" + cwd.substr(h.size());
                }
                char clockbuf[16];
                const auto wall = std::chrono::system_clock::now();
                const auto secs = std::chrono::system_clock::to_time_t(wall);
                std::tm tm{};
                ::localtime_r(&secs, &tm);
                std::snprintf(clockbuf, sizeof(clockbuf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
                              tm.tm_sec);
                sysinfo = sysinfo_line(sc, user, hostbuf, cwd, clockbuf);
            }
            std::vector<std::string> sidebar_rows;
            if (show_sidebar) {
                std::vector<std::string> raw = sidebar_result.rows;
                if (!sidebar_result.error.empty())
                    raw = {"sidebar: " + sidebar_result.error};
                else if (sidebar_running)
                    raw.push_back("(scanning...)");
                sidebar_rows.push_back(tint(sc, "1", "workspace"));
                for (const auto& row : raw)
                    sidebar_rows.push_back(tint(sc, "2", sanitize_display(row)));
            }
            const std::size_t sel = sidebar_focus && sidebar_sel < sidebar_result.rows.size()
                                        ? sidebar_sel + 1 // +1: header row shifts every entry
                                        : static_cast<std::size_t>(-1);
            screen.render(tail, last_user, raw_for_hint, composer_display, prompt, status,
                          streaming, cols, extra_rows, sysinfo, session_ids, sidebar_rows, sel,
                          provider_name_list(), model_choices);
        }
    }

done:
    exiting = true;
    abort_requested = true;
    if (worker.joinable())
        worker.join();
    if (models_worker.joinable())
        models_worker.join(); // bounded by its own 15 s curl timeout
    if (probe_worker.joinable())
        probe_worker.join(); // v4.3: bounded by its own 30 s curl timeout
    if (sidebar_worker.joinable())
        sidebar_worker.join(); // v4.13: git scan capped at 5 s
    logger->event("session_close",
                  {{"mode", "tui"}, {"id", session.meta.id}, {"temp", temp_mode ? "1" : "0"}});
    Logger::shutdown_all();
    screen.clear_live();
    if (temp_mode) {
        screen.commit("temp session " + session.meta.id + " discarded (nothing was saved)");
    } else {
        screen.commit("saved session " + session.meta.id + " (resume: mini-agent run --session " +
                      session.meta.id + ")");
    }
    return EXIT_SUCCESS;
}

} // namespace mini_agent::tui
