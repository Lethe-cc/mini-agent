// mini-agent CLI. Exit codes (design 007 section 5): 0 success,
// 1 runtime error (network/HTTP/protocol/io), 2 configuration error.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "cli_common.hpp"
#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/tui.hpp"

namespace mini_agent::cli {
namespace {
std::atomic<int> g_sigint_count{0};

void cli_sigint_handler(int) {
    // Async-signal-safe: relaxed store + re-raise. First Ctrl-C only sets
    // the flag; the second resets to SIG_DFL and re-raises so the default
    // disposition kills the process NOW (a wedged transport is a hard
    // stop, TUI parity). Deliberately NO SA_RESTART: blocking getlines
    // (approval prompt, line-mode input) must surface EINTR -- denial on
    // a failed approval read is exactly the Ctrl-C-during-approval
    // semantic, and an interrupted idle read takes the graceful
    // save/unlock exit path instead of a raw death.
    if (g_sigint_count.fetch_add(1, std::memory_order_relaxed) >= 1) {
        std::signal(SIGINT, SIG_DFL);
        std::raise(SIGINT);
    }
}
} // namespace

void install_cli_sigint() {
    // Gate: any terminal attached (stdin or stdout). Fully piped runs --
    // the scripted contract -- keep the default SIGINT disposition
    // (immediate process death) so nothing about automation changes.
    // Line mode runs with piped stdin but can still have a controlling
    // terminal (kill -INT), so stdout counts too.
    if (::isatty(STDIN_FILENO) != 1 && ::isatty(STDOUT_FILENO) != 1)
        return;
    g_sigint_count.store(0);
    struct sigaction act{};
    act.sa_handler = cli_sigint_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0; // no SA_RESTART: EINTR is our idle-interrupt channel
    ::sigaction(SIGINT, &act, nullptr);
}

// Sticky within a turn: every should_abort query in the plumbing (curl
// write/XFERINFO, subprocess poll slices, loop post-tool check) must see
// it until the turn unwinds. Cleared explicitly at the line-mode input
// boundary so an idle Ctrl-C does not poison the NEXT turn.
bool cli_abort_requested() {
    return g_sigint_count.load(std::memory_order_relaxed) > 0;
}

void reset_cli_sigint() {
    g_sigint_count.store(0, std::memory_order_relaxed);
}

bool cli_sigint_once() {
    return g_sigint_count.load(std::memory_order_relaxed) == 1;
}

void print_usage(std::ostream& out) {
    out << "mini-agent " MINI_AGENT_VERSION
        << " - a mini AI agent\n"
           "usage:\n"
           "  mini-agent ask [--tools M] [--cwd PATH] [--debug] \"question\"  one-shot to stdout\n"
           "  mini-agent ask [--tools M] --cwd PATH -   read question from stdin\n"
           "      --tools off|ask|allowlist|auto  (default off: non-interactive contract)\n"
           "          allowlist: only tool_allowlist names execute (script-safe, v3)\n"
           "      --provider NAME  use a named provider profile (v6; this session only)\n"
           "      --model NAME     model override; provider/model picks the profile too\n"
           "      --debug  alias of --log-level=debug (dump raw chat traffic)\n"
           "      --log-level info|debug|trace  traffic capture ladder (v4.8):\n"
           "          debug: raw chat request/response -> <log path>.debug\n"
           "          trace: + auxiliary traffic (models/probe, truncated)\n"
           "  mini-agent run [--session ID|PREFIX] [--new] [--temp] [--cwd PATH] [--debug]\n"
           "      session (TUI; v1 line-mode). Default reuses the most recently\n"
           "      updated session (v4.15); --new forces a fresh one; --temp opens a\n"
           "      fresh session that is never saved and is discarded on exit\n"
           "      --provider NAME / --model NAME  same overrides as ask (v6)\n"
           "  mini-agent                     bare == run: open the TUI directly (v4.9)\n"
           "      in-TUI session switch: /session <id|prefix> | /session new\n"
           "  mini-agent session list|rm ID  manage sessions\n"
           "  mini-agent session export ID [--format md|json] [--out PATH]\n"
           "  mini-agent session prune --older-than DAYS [--dry-run]\n"
           "      rm: ID may be the full session id or an unambiguous prefix\n"
           "  mini-agent mcp list            show configured MCP servers\n"
           "  --mcp NAME=SPEC (ask/run/mcp)  add one server this session:\n"
           "      NAME=https://host/mcp          Streamable HTTP\n"
           "      NAME=cmd arg1 arg2             stdio subprocess\n"
           "      NAME={json entry}              full control (env, bearer)\n"
           "  mini-agent version             version + commit + build time\n"
           "  mini-agent --help | --version  no network, no key needed\n"
           "\nenvironment:\n"
           "  MINI_AGENT_BASE_URL  e.g. https://api.openai.com/v1\n"
           "  MINI_AGENT_API_KEY   never printed\n"
           "  MINI_AGENT_MODEL     model name, or provider/model to pick a profile\n";
}

bool read_stdin(std::string* out) {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    *out = buffer.str();
    return !out->empty();
}

// Merged env+file config; on incompleteness return false with hints.
bool load_app_config(mini_agent::AppConfig* out, std::string* config_error) {
    std::string file_error;
    *out = mini_agent::load_config_merged(&file_error);
    if (!file_error.empty()) {
        std::cerr << "warn: " << file_error << "\n"; // proceed with env/partial
    }
    if (!out->complete()) {
        std::string missing;
        for (const auto& name : out->missing()) {
            if (!missing.empty())
                missing += ", ";
            missing += name;
        }
        *config_error = "LLM config incomplete (missing " + missing +
                        "). Option 1: environment variables "
                        "MINI_AGENT_BASE_URL/MINI_AGENT_API_KEY/MINI_AGENT_MODEL; option 2: run "
                        "mini-agent and use /provider <url>, /login <key>, /model <name>.";
        return false;
    }
    return true;
}

mini_agent::SessionStore make_store() {
    auto root = mini_agent::default_session_root();
    if (const char* override_root = std::getenv("MINI_AGENT_SESSION_DIR");
        override_root && *override_root) {
        root = override_root;
    }
    return mini_agent::SessionStore(std::move(root));
}

void print_provider_error(const mini_agent::ProviderError& error) {
    const char* prefix = "error: ";
    switch (error.kind()) {
    case mini_agent::ProviderErrorKind::Network:
        prefix = "network error: ";
        break;
    case mini_agent::ProviderErrorKind::HttpStatus:
        prefix = "http error: ";
        break;
    case mini_agent::ProviderErrorKind::Protocol:
        prefix = "protocol error: ";
        break;
    case mini_agent::ProviderErrorKind::Cancelled:
        prefix = "cancelled: ";
        break;
    }
    std::cerr << prefix << error.what() << '\n';
    // 015 (design 015 §4B) + 017: an overflow-shaped 400 (not a throttling
    // rejection in overflow wording) gets the same guidance as the TUI
    // hint (the CLI has no /compact; point at the TUI command).
    if (error.kind() == mini_agent::ProviderErrorKind::HttpStatus && error.http_status() == 400 &&
        !mini_agent::overflow_excluded(error.what()) &&
        mini_agent::context_overflow_message(error.what())) {
        std::cerr << "hint: context window exceeded · run the TUI and use /compact to "
                     "summarize the session history, or start a fresh session\n";
    }
}

// 014 (design 014 §5): provider config whose retry notification logs a
// metadata-only event. The CLI logger is stable for the whole process
// (no /log command here), so a reference capture is safe.
mini_agent::ProviderConfig provider_with_retry_log(const mini_agent::AppConfig& config,
                                                   mini_agent::Logger& logger) {
    auto pc = config.to_provider();
    pc.on_retry = [&logger](int attempt, int http_status, int wait_ms) {
        logger.event("provider_retry", {{"attempt", std::to_string(attempt)},
                                        {"status", std::to_string(http_status)},
                                        {"wait_ms", std::to_string(wait_ms)}});
    };
    return pc;
}

// Apply repeatable --mcp NAME=SPEC flags over merged config (session-
// scoped injection, design 008 section 5.3): upsert so a CLI name replaces
// a same-named file entry. Returns false on the first invalid spec.
bool apply_mcp_flags(int argc, char** argv, mini_agent::AppConfig* config, std::string* error) {
    for (int i = 0; i < argc; ++i) {
        std::string_view value;
        if (std::strcmp(argv[i], "--mcp") == 0 && i + 1 < argc) {
            value = argv[i + 1];
        } else if (std::strncmp(argv[i], "--mcp=", 6) == 0) {
            value = std::string_view(argv[i]).substr(6);
        } else {
            continue;
        }
        mini_agent::McpServerConfig server;
        if (!mini_agent::parse_mcp_cli_spec(value, &server, error))
            return false;
        mini_agent::upsert_mcp_server(config, server);
    }
    return true;
}

// Apply --provider NAME / --model NAME session overrides (design 012
// section 5.2, v7 semantics per design 013 §5.3) on the merged config,
// BEFORE the completeness gate so a stored profile can supply the values
// env/file leave empty. --provider selects a named profile; --model is a
// session overlay on current (never written to a profile or to disk) and
// may itself be qualified (provider/model selects that profile first).
bool apply_provider_flags(int argc, char** argv, mini_agent::AppConfig* config,
                          std::string* error) {
    std::string provider_flag;
    std::string model_flag;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider_flag = argv[++i];
        } else if (std::strncmp(argv[i], "--provider=", 11) == 0) {
            provider_flag = argv[i] + 11;
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_flag = argv[++i];
        } else if (std::strncmp(argv[i], "--model=", 8) == 0) {
            model_flag = argv[i] + 8;
        }
    }
    auto available = [&]() {
        std::string names;
        for (const auto& profile : config->providers) {
            if (!names.empty())
                names += ", ";
            names += profile.name;
        }
        return names.empty() ? std::string("(none; add one via /provider add <name>)") : names;
    };
    if (!provider_flag.empty() && !config->activate_provider(provider_flag)) {
        *error = "unknown provider '" + provider_flag + "' (available: " + available() + ")";
        return false;
    }
    if (!model_flag.empty()) {
        std::string provider_part;
        std::string model_part;
        if (mini_agent::split_qualified_model(model_flag, &provider_part, &model_part) &&
            config->find_provider(provider_part)) {
            if (!config->activate_provider(provider_part)) {
                *error = "unknown provider '" + provider_part + "'";
                return false;
            }
            config->session_model = model_part;
        } else {
            // Unknown or absent prefix: a literal model id (ids may
            // contain '/', e.g. openrouter names).
            config->session_model = model_flag;
        }
        config->refresh_current();
    }
    return true;
}
} // namespace mini_agent::cli
