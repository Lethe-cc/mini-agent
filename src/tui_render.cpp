// Pure TUI rendering/completion helpers, split out of tui.cpp (2026-11-14
// maintainability pass): everything here is a pure function over its
// inputs -- no terminal state, no threads, no I/O -- and unit-tested via
// the wrappers declared in mini_agent/tui.hpp. tui.cpp keeps the frame
// loop (LiveScreen + run_loop) and the terminal/RAII plumbing.
#include "mini_agent/tui.hpp"

#include "mini_agent/highlight.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mini_agent::tui {

constexpr std::string_view kCommands[] = {
    "/approve",   "/compact", "/config",   "/context", "/cwd",     "/debug",   "/exit",
    "/help",      "/login",   "/mcp",      "/model",   "/models",  "/preview", "/provider",
    "/reasoning", "/session", "/sessions", "/sidebar", "/sysinfo", "/tools"};

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
    const int set = (config.current.base_url.empty() ? 0 : 1) + (config.current.api_key.empty() ? 0 : 1) +
                    (config.current.model.empty() ? 0 : 1);
    // Only the next step, in provider -> key -> model order: each command is
    // independently saved, so any of them can be supplied in its own sitting
    // (or from a different process) and this line reflects what is left.
    std::string next;
    if (config.current.base_url.empty())
        next = "/provider <url>";
    else if (config.current.api_key.empty())
        next = "/login <key>";
    else if (config.current.model.empty())
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
    if (config.current.base_url.empty() || config.current.api_key.empty())
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
        // cols-1 wrap budget: the autowrap guard (see compose_footer).
        const auto rendered = md::render(preview, cols > 1 ? cols - 1 : 1);
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
                            "y=allow  n=deny  a=allow ALL (session)  ?=params  (Esc/Ctrl-C "
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
                        expanded ? "y=allow  n=deny  a=allow ALL (session)  ?=params(compact)  "
                                   "other keys held"
                                 : "y=allow  n=deny  a=allow ALL (session)  ?=params  "
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
    const std::size_t split = cols - side_w - 4; // content width + " | " + autowrap guard
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
    const std::size_t guard = cols > 1 ? cols - 1 : 1;
    if (right.empty())
        return md::clip(std::string(left), guard);
    const std::string info = md::clip(std::string(right), guard);
    const std::size_t rw = md::display_width(info);
    if (rw + 1 >= cols)
        return info; // no room for both: the info wins
    // Composed width caps at cols-1, never cols: a row filling the last
    // column leaves the terminal in deferred-autowrap, and the frame's
    // trailing \n then advances TWO rows, breaking the live_rows_ cursor
    // accounting (5.6 rule 2). One blank column is the guard.
    const std::size_t budget = cols - rw - 2; // min one-space gutter + guard
    std::string head = md::clip(std::string(left), budget);
    const std::size_t hw = md::display_width(head);
    head.append(cols - 1 - rw - hw, ' ');
    return head + info;
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

} // namespace mini_agent::tui
