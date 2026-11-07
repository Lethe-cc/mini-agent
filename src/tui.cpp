// Self-drawn minimal TUI (ADR 003) implementing the frame model and
// interaction contract of design 007 section 5.6.
//
// Rendering (5.6 rule 2; supersedes 5.4's "alternate screen" note): inline
// main-screen rendering. Committed history prints once and lives in the
// terminal's native scrollback; only a small "live region" (response tail,
// composer, status) is redrawn in place. No alternate screen, no full diff.
#include "mini_agent/tui.hpp"

#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    "/approve", "/config", "/context", "/cwd",      "/exit",      "/help",     "/login",
    "/mcp",     "/model",  "/models",  "/provider", "/reasoning", "/sessions", "/tools"};

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

// ---- delta buffer: worker appends, main drains per frame (5.6 rule 1) ----
class DeltaBuffer {
  public:
    void append(std::string_view delta) {
        std::lock_guard<std::mutex> lock(mutex_);
        text_.append(delta);
    }
    std::string snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        text_.clear();
    }

  private:
    std::mutex mutex_;
    std::string text_;
};

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

std::optional<std::string> slash_tab_complete(std::string_view line, int& cursor_state) {
    if (line.empty() || line.front() != '/')
        return std::nullopt;
    const auto matches = slash_matches(line);
    if (matches.empty())
        return std::nullopt;
    const std::string_view pick = matches[static_cast<std::size_t>(cursor_state) % matches.size()];
    cursor_state = (cursor_state + 1) % static_cast<int>(matches.size());
    return std::string(pick);
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

std::vector<std::string> slash_completion_hint(std::string_view composer) {
    const bool colors = colors_enabled();
    if (composer.empty() || composer.front() != '/')
        return {};
    const auto matches = slash_matches(composer);
    std::vector<std::string> rows;
    if (matches.empty()) {
        rows.emplace_back(tint(colors, "33", "Tab completion: no matching command (see /help)"));
        return rows;
    }
    rows.emplace_back(tint(colors, "2", "Tab completion (cycle with Tab):"));
    for (const auto& cmd : matches)
        rows.push_back(tint(colors, "2", "  " + std::string(cmd)));
    return rows;
}

// ------------------------------------------------------------ v2 tool UI

std::string tool_activity_line(const std::string& tool_name, const std::string& arguments_json,
                               bool executed, bool is_error, long long elapsed_ms,
                               std::size_t result_bytes) {
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
            // Colour map mirrors `git diff` defaults (2026-11-05 maintainer
            // reference screenshot): file headers bold meta, hunk header
            // cyan, removed red, added green, context left terminal-default.
            std::string tinted;
            const bool meta_header = line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0;
            if (meta_header)
                tinted = tint(colors, "1", sanitize_display(line));
            else if (!line.empty() && line[0] == '+')
                tinted = tint(colors, "32", sanitize_display(line));
            else if (!line.empty() && line[0] == '-')
                tinted = tint(colors, "31", sanitize_display(line));
            else if (!line.empty() && line[0] == '@')
                tinted = tint(colors, "36", sanitize_display(line));
            else if (!line.empty() && line[0] == ':')
                tinted = tint(colors, "2", sanitize_display(line));
            else
                tinted = std::string(sanitize_display(line)); // context: plain
            rows.push_back(std::move(tinted));
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

std::string status_usage_segments(bool colors, const StatusInputs& inputs) {
    // Pi-style compact token counts: <1k raw, <1M one-decimal k, else M.
    auto format_token_count = [](long long n) {
        if (n < 1000)
            return std::to_string(n);
        char buf[32];
        if (n < 1000000)
            std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1000.0);
        else
            std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
        return std::string(buf);
    };
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
                std::size_t cols,
                const std::vector<std::string>& extra_rows =
                    {}) { // NOLINT(bugprone-easily-swappable-parameters)
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
            status == last_status_ && extra_rows == last_extra_) {
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
        const std::size_t cap = rows > 2 ? rows - 2 : 1; // block budget minus status row
        auto pin_rows =
            streaming
                ? md::wrap(style_history_line("You: " + pinned_question, colors_enabled()), cols)
                : std::vector<std::string>{};
        auto tail_rows = streaming ? md::wrap(response_tail, cols) : std::vector<std::string>{};
        // The prompt marker carries the provider connection state (2026-11-02
        // request): same row as the composer, one display column wide.
        auto comp_rows = md::wrap(prompt + " " + composer_display, cols);
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
            hint_source = slash_completion_hint(composer);
        }
        for (const auto& line : hint_source) {
            if (hint_rows.size() >= budget)
                break;
            for (auto& wrapped : md::wrap(line, cols)) {
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
        // Status is always one row; the trailing reset matters now that the
        // row carries SGR segments: md::clip can cut mid-span, and this is
        // the block's last row, so an unclosed span would leak into the next
        // repaint's rows (5.6 rule 2: no style may survive a row boundary).
        content.push_back(md::clip(status, cols) + "\x1b[0m");
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

namespace {

// One decided tool invocation parked for the main thread (worker -> UI
// handoff: rounds are committed to history only on the main thread, same
// rule as models_result/deltas -- 5.6 rule 1).
struct ToolRound {
    std::string tool_name;
    std::string arguments_json;
    bool executed = false;
    bool is_error = false;
    long long elapsed_ms = 0;
    std::size_t result_bytes = 0;
    bool approval_timeout = false;
};

// Fixed approval slot (no dynamic ownership races): the worker fills the
// call fields, publishes request_seq, and waits for answer_seq to catch
// up. The main thread reads/writes only through the sequence numbers, so
// the object never moves and no pointer can dangle.
struct ApprovalSlot {
    std::mutex mutex; // guards `call` payload writes
    ToolCall call;    // valid between request and answer
    // v3 (design 009 5.3): pre-computed diff preview for write-side tools,
    // written under the same mutex BEFORE request_seq publishes, so the UI
    // thread never sees a pending approval without its preview settled.
    std::string preview;
    std::atomic<unsigned long long> request_seq{0};
    std::atomic<unsigned long long> answer_seq{0};
    std::atomic<int> answer{0}; // 1 allow, 2 deny, 3 allow-session
};

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
    DeltaBuffer deltas;
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
    logger->event("session_open", {{"mode", "tui"},
                                   {"id", options.session_id},
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
    } models_result;
    AuthProbe auth;                 // latest login/auth verdict, shown by /config too
    std::string models_fingerprint; // base_url+'\n'+api_key of the last fetch (not a hash)
    std::chrono::steady_clock::time_point models_start;
    bool model_menu_open = false;
    std::vector<std::string> model_choices; // ids backing the open menu
    // v4.2 (design 010 §11): capability metadata from the last successful
    // /models fetch (main-thread only; the worker fills models_result.caps).
    std::vector<ModelCapabilities> model_caps;
    bool reasoning_menu_open = false;
    std::vector<std::string> reasoning_choices; // efforts backing the open menu
    int tab_state = 0;
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
    auto turn_start = steady_now();
    auto last_ctrl_c = std::chrono::steady_clock::time_point{};

    const auto banner_model = config.model.empty() ? std::string("(no model)") : config.model;
    screen.commit("mini-agent · model=" + banner_model + " · session=" + options.session_id);
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
    std::mutex tool_mutex;
    std::vector<ToolRound> tool_events;
    ApprovalSlot approval_slot;
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
        std::vector<ToolRound> batch;
        {
            std::lock_guard<std::mutex> lock(tool_mutex);
            batch.swap(tool_events);
        }
        for (const auto& round : batch) {
            screen.commit(tool_activity_line(round.tool_name, round.arguments_json, round.executed,
                                             round.is_error, round.elapsed_ms, round.result_bytes));
            tools_active = true;
        }
    };

    auto approval_pending = [&] {
        return approval_slot.request_seq.load() != approval_slot.answer_seq.load();
    };
    // Answer the pending approval with the UI key semantics (008 5.4):
    // y allow once, n deny, a allow-name for the session, Esc/Ctrl-C deny.
    auto answer_approval = [&](int key) {
        std::lock_guard<std::mutex> lock(approval_slot.mutex);
        ToolCall call = approval_slot.call; // snapshot under the slot mutex
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
        approval_slot.answer.store(answer);
        approval_slot.answer_seq.store(approval_slot.request_seq.load());
        logger->event("tool_approve",
                      {{"tool", call.name},
                       {"decision", answer == 1 ? "allow" : (answer == 3 ? "session" : "deny")}});
    };

    auto start_turn = [&](const std::string& text) {
        if (worker.joinable())
            worker.join();
        config_snapshot = config; // refresh after join, before worker start
        last_user = text;
        deltas.clear();
        abort_requested = false;
        turn_done = false;
        result = TurnResult{};
        {
            std::lock_guard<std::mutex> lock(tool_mutex);
            tool_events.clear();
        }
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
            OpenAiProvider provider(turn_config.to_provider());
            Chat outgoing;
            for (const auto& m : session.chat.messages())
                outgoing.add(Message{m});
            outgoing.add(Message{user_msg});
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
                {
                    std::lock_guard<std::mutex> lock(approval_slot.mutex);
                    approval_slot.call = call;
                    // v3: compute the diff preview on the worker thread
                    // (registry preview honors the same single-owner
                    // contract as list/call) and arm the runtime's race
                    // guard -- both inside the mutex, before publish.
                    approval_slot.preview = registry ? registry->preview(call) : std::string();
                    seq = approval_slot.request_seq.load() + 1;
                    approval_slot.request_seq.store(seq); // publish (call set first)
                }
                const auto wait_start = std::chrono::steady_clock::now();
                int answer = 0;
                while (approval_slot.answer_seq.load() != seq) {
                    if (abort_requested.load())
                        break;
                    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::steady_clock::now() - wait_start)
                                            .count();
                    if (waited >= 120) { // never block the worker forever
                        std::lock_guard<std::mutex> lock(tool_mutex);
                        ToolRound timeout_round;
                        timeout_round.tool_name = call.name;
                        timeout_round.arguments_json = call.arguments_json;
                        timeout_round.executed = false;
                        timeout_round.is_error = true;
                        timeout_round.approval_timeout = true;
                        tool_events.push_back(std::move(timeout_round));
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                answer = abort_requested.load() && approval_slot.answer_seq.load() != seq
                             ? 2
                             : approval_slot.answer.load();
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
            turn_options.on_tool = [&](const ToolCall& call, const ToolResult& tool_result,
                                       bool executed) {
                ToolRound round;
                round.tool_name = call.name;
                round.arguments_json = call.arguments_json;
                round.executed = executed;
                round.is_error = tool_result.is_error;
                round.elapsed_ms = tool_result.elapsed_ms;
                round.result_bytes = tool_result.text.size();
                std::lock_guard<std::mutex> lock(tool_mutex);
                tool_events.push_back(std::move(round));
            };
            // v4: run_turn writes here only on a successful turn; the
            // worker copies it into `result` (join-then-read discipline).
            TokenUsage turn_usage;
            turn_options.usage_out = &turn_usage;
            ActualReasoning turn_reasoning; // v4.2
            turn_options.reasoning_out = &turn_reasoning;
            try {
                // Run the (possibly multi-round) turn on a detached copy:
                // run_turn commits the whole turn to its Chat only on
                // success; on error we discard the copy, so session.chat
                // stays at the previous turn boundary exactly as in v1
                // (5.6 rule: errors never reach storage; 008 section 4).
                Chat turn_chat;
                for (const auto& m : session.chat.messages())
                    turn_chat.add(Message{m});
                r.reply = run_turn(
                    turn_chat, provider, user_msg.content,
                    [&](std::string_view d) { deltas.append(d); },
                    [&] { return abort_requested.load(); }, turn_options);
                session.chat = std::move(turn_chat);
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
    // No-op unless base_url+api_key are both set and differ from the last
    // attempt. Logs only metadata (status/count/elapsed): the api_key never
    // reaches the log line, the menu, or any ProviderError text.
    auto start_models_fetch = [&](bool force) {
        if (models_running || config.base_url.empty() || config.api_key.empty())
            return;
        const std::string fingerprint = config.base_url + '\n' + config.api_key;
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
                      {{"status", "start"}, {"base_url", Logger::redact_url(config.base_url)}});
        const AppConfig snapshot = config; // thread boundary (like start_turn)
        // The worker computes only the result and sets the flag: `logger` and
        // `screen` are main-thread state (/log reassigns logger), so the
        // completion event and the menu render happen on the main thread.
        models_worker = std::thread([&, snapshot] {
            ModelsResult r;
            // Bounded wait: a hung /models must not stall the TUI for the
            // full chat timeout budget (design 007 section 6 timeouts).
            ProviderConfig pc = snapshot.to_provider();
            pc.connect_timeout_seconds = 5;
            pc.total_timeout_seconds =
                pc.total_timeout_seconds < 15 ? pc.total_timeout_seconds : 15;
            try {
                OpenAiProvider provider(pc);
                // v4.2: one GET carries both the id list and the optional
                // capability metadata (OpenRouter-style gateways). Gateways
                // without the fields simply yield empty caps -- ids stay.
                r.caps = provider.list_model_capabilities();
                for (const auto& cap : r.caps)
                    r.ids.push_back(cap.id);
                r.ok = true;
            } catch (const ProviderError& e) {
                // parse_models_meta_json shares parse_models_json's fail-
                // closed root/data guards, so a Protocol error here would
                // fail the id-only path identically -- no fallback needed.
                r.error = e.what();
                r.http_status = e.http_status();
                r.network_error = e.kind() == ProviderErrorKind::Network;
            } catch (const std::exception& e) {
                r.error = e.what();
            }
            models_result = std::move(r);
            models_done = true;
        });
    };

    // Called after /provider or /login mutates config: auto-discover once
    // both halves are known (the user's stated request).
    auto maybe_autofetch_models = [&] {
        if (!config.base_url.empty() && !config.api_key.empty()) {
            screen.commit("[info] listing models from provider...");
            start_models_fetch(false);
        }
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
            screen.commit("          /tools (tool registry) · /mcp (servers) · /cwd [path]");
            screen.commit("          /approve ask|allowlist|off (tool gate)");
            screen.commit("          /models (list & pick) · /log <path>|off|default");
            screen.commit("          /config · /sessions · /exit");
            screen.commit("Each of /provider /login /model saves on its own;");
            screen.commit("configure them in any order, any number of sittings.");
            screen.commit("prompt: ?=unconfigured/unverified · ~=checking · >=ok");
            screen.commit("        !=key rejected · x=provider unreachable");
            screen.commit("Enter sends · Enter queues while streaming");
            return true;
        }
        if (line.rfind("/login", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                secret_input = true; // masked composer; next Enter sets key
                secret_field = "api_key";
                screen.commit("[input] API key (hidden); Enter confirms, empty Enter cancels");
            } else {
                config.api_key = std::string(arg);
                auth = AuthProbe{}; // previous verdict belongs to the old key
                save_and_report(true);
                maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/models", 0) == 0) {
            // Listed before /model so the shared prefix resolves here first.
            if (config.base_url.empty() || config.api_key.empty()) {
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
                screen.commit("model=" + (config.model.empty() ? "(not set)" : config.model) +
                              "; usage /model <name>" +
                              (config.base_url.empty() || config.api_key.empty()
                                   ? std::string()
                                   : " · /models lists what the provider offers"));
            } else {
                config.model = std::string(arg);
                session.meta.model = config.model;
                reset_usage_readings(); // v4: window/counts belong to the old model
                save_and_report(true);
            }
            return true;
        }
        if (line.rfind("/provider", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(
                    "base_url=" + (config.base_url.empty() ? "(not set)" : config.base_url) +
                    "; usage /provider <url>");
            } else {
                config.base_url = std::string(arg);
                auth = AuthProbe{}; // a different endpoint cannot inherit it
                save_and_report(true);
                maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/log", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(logger->enabled() ? "logging on: " + config.log_file
                                                : "logging off; usage /log <path> or /log off");
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
            screen.commit("log_file=" + (config.log_file.empty() ? "(off)" : config.log_file));
            // v4 fields (design 010).
            screen.commit("context_window=" + (config.context_window > 0
                                                   ? std::to_string(config.context_window)
                                                   : std::string("(not set)")));
            screen.commit("reasoning_effort=" + (config.reasoning_effort.empty()
                                                     ? std::string("(not set)")
                                                     : config.reasoning_effort));
            screen.commit("config file: " + default_config_path().string() +
                          " (env overrides file)");
            return true;
        }
        if (line == "/sessions") {
            std::string err;
            const auto ids = store.list(&err);
            screen.commit(ids.empty() ? "sessions: (empty)" : "sessions:");
            for (const auto& id : ids)
                screen.commit("  " + id);
            screen.commit("resume after exit: mini-agent run --session <id>");
            return true;
        }
        if (line.rfind("/context", 0) == 0) {
            // v4 (design 010): model context window for the status-bar
            // ctx% reading. Config is authoritative -- no probing.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const auto inputs = context_reading();
                std::string shown = config.context_window > 0
                                        ? std::to_string(config.context_window)
                                        : std::string("(not set)");
                screen.commit("context_window=" + shown +
                              "; usage /context <tokens> or /context off");
                screen.commit("ctx reading: " +
                              (inputs.context_tokens > 0
                                   ? (inputs.context_estimated ? "~" : "") +
                                         std::to_string(inputs.context_tokens) + " tokens" +
                                         (config.context_window > 0
                                              ? " = " +
                                                    std::to_string(inputs.context_tokens * 100 /
                                                                   config.context_window) +
                                                    "% of window"
                                              : std::string())
                                   : std::string("(no data yet)")) +
                              (status_usage_reported ? " · provider-reported" : " · estimated"));
            } else if (arg == "off" || arg == "0") {
                config.context_window = 0;
                save_and_report();
                screen.commit("[ok] context window cleared · status shows tokens, no %");
            } else {
                const long long parsed_window = std::strtoll(std::string(arg).c_str(), nullptr, 10);
                if (parsed_window <= 0) {
                    screen.commit("[error] /context expects a positive token count");
                    return true;
                }
                config.context_window = parsed_window;
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
                save_and_report();
                screen.commit("[ok] reasoning effort cleared · requests omit the field");
            } else {
                config.reasoning_effort = std::string(arg);
                save_and_report();
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
            screen.commit("[ok] workspace=" + sanitize_display(config.workspace) +
                          " · tools rebound · " +
                          (config.workspace == std::filesystem::current_path().string()
                               ? "(matches launch cwd)"
                               : "run_bash now starts inside it"));
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
                            switch (fin) {
                            case 'A':
                                hist_up();
                                break;
                            case 'B':
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
                    if (secret_input) {
                        secret_input = false;
                        if (!composer.empty()) {
                            if (secret_field == "api_key")
                                config.api_key = composer;
                            auth = AuthProbe{}; // stale the moment it changes
                            screen.commit("[ok] api_key updated");
                            save_and_report(true);
                            maybe_autofetch_models();
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
                                config.model = *pick;
                                session.meta.model = config.model;
                                reset_usage_readings(); // v4
                                save_and_report();
                                logger->event("model_select",
                                              {{"model", config.model}, {"source", "menu"}});
                                screen.commit("model=" + config.model);
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
                    if (const auto completed = slash_tab_complete(composer, tab_state)) {
                        composer = *completed;
                        cursor = composer.size();
                        mark_edited();
                    }
                } else if (c == 0x7F || c == 0x08) {
                    if (utf8_erase_before(composer, cursor))
                        mark_edited();
                } else if (c >= 0x20) {
                    utf8_insert(composer, cursor, std::string_view(&buf[i], 1));
                    mark_edited();
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
            if (models_result.ok) {
                logger->event("models_list", {{"status", "ok"},
                                              {"count", std::to_string(models_result.ids.size())},
                                              {"elapsed_ms", std::to_string(ms)}});
                model_choices = models_result.ids;
                model_caps = std::move(models_result.caps); // v4.2
                models_result.caps.clear();
                model_menu_open = !model_choices.empty();
                if (model_menu_open) {
                    for (const auto& row : model_menu_rows(model_choices, config.model))
                        screen.commit(row);
                } else {
                    screen.commit("[info] provider listed no models; use /model <name>");
                }
            } else {
                logger->event("models_list", {{"status", "error"},
                                              {"count", "0"},
                                              {"elapsed_ms", std::to_string(ms)},
                                              {"error", models_result.error}});
                screen.commit("[error] model list failed: " + models_result.error);
            }
            // The request we just made is also the credential probe: report
            // whether the provider accepted the key, separately from whether
            // the listing itself worked (a 401 means re-enter /login, whereas
            // a 200 with an unparseable body still proves the key is good).
            auth.attempted = true;
            auth.fetch_ok = models_result.ok;
            auth.http_status = models_result.http_status;
            auth.network_error = models_result.network_error;
            auth.model_count = models_result.ids.size();
            screen.commit(auth_status_line(auth));
            std::vector<std::pair<std::string, std::string>> login_fields = {
                {"status", std::string(auth_log_token(auth))},
                {"models", std::to_string(auth.model_count)},
            };
            // Only report a status code when one actually arrived: a success
            // path never raised one, and writing `http_status=0` there would
            // read like a bogus response rather than "not applicable".
            if (auth.http_status != 0)
                login_fields.emplace_back("http_status", std::to_string(auth.http_status));
            logger->event("login", login_fields);
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
                if (!store.save(session, &err)) {
                    screen.commit("[warn] save failed: " + err);
                    logger->event("session_save", {{"status", "error"}, {"error", err}});
                }
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
            // The prompt marker is the user-facing feedback (2026-11-02: the
            // [auth] history row that used to go with this is deliberate noise
            // on every first reply), so a chat-driven verdict change is only
            // recorded in the diagnostics log.
            if (auth_log_token(auth) != login_before) {
                std::vector<std::pair<std::string, std::string>> login_fields = {
                    {"status", std::string(auth_log_token(auth))},
                    {"models", std::to_string(auth.model_count)},
                    {"source", "chat"},
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
            const std::string tail = streaming ? deltas.snapshot() : std::string{};
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
            std::string status =
                tint(sc, "2",
                     "model=" + (config.model.empty() ? std::string("(not set)") : config.model) +
                         " · session=" + options.session_id);
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
                unsigned long long seq = approval_slot.request_seq.load();
                ToolCall snapshot;
                std::string snapshot_preview;
                {
                    std::lock_guard<std::mutex> lock(approval_slot.mutex);
                    (void)seq;
                    snapshot = approval_slot.call;
                    snapshot_preview = approval_slot.preview;
                }
                status += tint(sc, "1;33", " · AWAITING APPROVAL");
                extra_rows = approval_rows(snapshot.name, snapshot.arguments_json,
                                           approval_expanded, snapshot_preview);
            }
            screen.render(tail, last_user, raw_for_hint, composer_display, prompt, status,
                          streaming, cols, extra_rows);
        }
    }

done:
    exiting = true;
    abort_requested = true;
    if (worker.joinable())
        worker.join();
    if (models_worker.joinable())
        models_worker.join(); // bounded by its own 15 s curl timeout
    logger->event("session_close", {{"mode", "tui"}, {"id", session.meta.id}});
    Logger::shutdown_all();
    screen.clear_live();
    screen.commit("saved session " + session.meta.id + " (resume: mini-agent run --session " +
                  session.meta.id + ")");
    return EXIT_SUCCESS;
}

} // namespace mini_agent::tui
