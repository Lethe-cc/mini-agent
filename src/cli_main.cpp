// mini-agent CLI. Exit codes (design 007 section 5): 0 success,
// 1 runtime error (network/HTTP/protocol/io), 2 configuration error.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/tui.hpp"

#ifndef MINI_AGENT_VERSION
#define MINI_AGENT_VERSION "0.0.0-dev"
#endif

namespace {

constexpr int kOk = 0;
constexpr int kRuntimeError = 1;
constexpr int kConfigError = 2;

// CLI interrupt channel (v5, design 007 5.6): TTY-only SIGINT handler
// feeding the existing should_abort plumbing (curl write/XFERINFO
// callbacks, subprocess poll slices). First Ctrl-C requests a clean
// cancel: run_turn throws Cancelled at the next checkpoint, history
// stays at the previous turn boundary, session lock is released on the
// normal return path. A second Ctrl-C restores SIG_DFL so a wedged
// transport can still be killed outright (escape hatch, TUI parity).
// Non-TTY pipes keep the default disposition: no behavior change for
// the byte-identical script contract (008 5.4).
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
           "      rm: ID may be the full session id or an unambiguous prefix\n"
           "  mini-agent mcp list            show configured MCP servers\n"
           "  --mcp NAME=SPEC (ask/run/mcp)  add one server this session:\n"
           "      NAME=https://host/mcp          Streamable HTTP\n"
           "      NAME=cmd arg1 arg2             stdio subprocess\n"
           "      NAME={json entry}              full control (env, bearer)\n"
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

// mcp list: print configured servers after merged file/env config, with
// --mcp flags upserted on top (same precedence ask/run use). Secrets are
// always redacted: bearer shows "set" only, URLs go through redact_url.
int cmd_mcp(int argc, char** argv) {
    if (!(argc >= 3 && std::strcmp(argv[2], "list") == 0)) {
        std::cerr << "usage: mini-agent mcp list [--mcp NAME=SPEC ...]\n";
        return kConfigError;
    }
    // Merged config WITHOUT the completeness gate: listing servers must
    // work before base_url/key/model exist (a fresh setup step).
    std::string file_error;
    mini_agent::AppConfig config = mini_agent::load_config_merged(&file_error);
    if (!file_error.empty()) {
        std::cerr << "error: " << file_error << '\n';
        return kConfigError;
    }
    std::string apply_error;
    if (!apply_mcp_flags(argc, argv, &config, &apply_error)) {
        std::cerr << "error: " << apply_error << '\n';
        return kConfigError;
    }
    if (config.mcp_servers.empty()) {
        std::cout << "mcp servers: (none)\n"
                  << "  add via config.json mcp_servers, or --mcp NAME=SPEC\n";
        return kOk;
    }
    for (const auto& server : config.mcp_servers) {
        if (server.transport == "http") {
            std::cout << server.name << "  http  " << mini_agent::Logger::redact_url(server.url);
            if (!server.bearer_token.empty())
                std::cout << "  bearer=set";
            std::cout << '\n';
        } else {
            std::cout << server.name << "  stdio  " << server.command;
            for (const auto& arg : server.args)
                std::cout << ' ' << arg;
            std::cout << '\n';
        }
    }
    return kOk;
}

int cmd_ask(int argc, char** argv) {
    mini_agent::AppConfig config;
    std::string config_error;
    const bool configured = load_app_config(&config, &config_error);
    std::string provider_error;
    if (!apply_provider_flags(argc, argv, &config, &provider_error)) {
        std::cerr << "error: " << provider_error << '\n';
        return kConfigError;
    }
    // v6 (012): the flags may complete an otherwise partial config (a
    // stored profile supplies url/key); re-gate after applying them.
    if (!configured && !config.complete()) {
        std::cerr << config_error << '\n';
        return kConfigError;
    }
    std::string question;
    std::string tools_mode = "off"; // non-interactive default (008 R4)
    std::string mcp_error;
    if (!apply_mcp_flags(argc, argv, &config, &mcp_error)) {
        std::cerr << "error: " << mcp_error << '\n';
        return kConfigError;
    }
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tools") == 0 && i + 1 < argc) {
            tools_mode = argv[++i];
            continue;
        }
        if (std::strncmp(argv[i], "--tools=", 8) == 0) {
            tools_mode = argv[i] + 8;
            continue;
        }
        if (std::strcmp(argv[i], "--mcp") == 0 && i + 1 < argc) {
            ++i; // handled by apply_mcp_flags
            continue;
        }
        if (std::strncmp(argv[i], "--mcp=", 6) == 0)
            continue; // handled by apply_mcp_flags
        if ((std::strcmp(argv[i], "--provider") == 0 || std::strcmp(argv[i], "--model") == 0) &&
            i + 1 < argc) {
            ++i; // handled by apply_provider_flags
            continue;
        }
        if (std::strncmp(argv[i], "--provider=", 11) == 0 ||
            std::strncmp(argv[i], "--model=", 8) == 0)
            continue; // handled by apply_provider_flags
        if (std::strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            config.workspace = argv[++i]; // v3 workspace override (009 5.4)
            continue;
        }
        if (std::strncmp(argv[i], "--cwd=", 6) == 0) {
            config.workspace = argv[i] + 6;
            continue;
        }
        if (std::strcmp(argv[i], "--debug") == 0) {
            config.log_level = "debug"; // v4.4 alias (010 §19)
            continue;
        }
        if (std::strcmp(argv[i], "--debug=all") == 0) {
            config.log_level = "trace"; // v4.6 alias (010 §19)
            continue;
        }
        if (std::strncmp(argv[i], "--log-level=", 12) == 0) {
            config.log_level = argv[i] + 12; // v4.8 (010 §19)
            continue;
        }
        if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            config.log_level = argv[++i]; // v4.8 (010 §19)
            continue;
        }
        positional.emplace_back(argv[i]);
    }
    if (tools_mode != "off" && tools_mode != "ask" && tools_mode != "auto" &&
        tools_mode != "allowlist") {
        std::cerr << "error: --tools must be off|ask|allowlist|auto\n";
        return kConfigError;
    }
    if (config.log_level != "info" && config.log_level != "debug" && config.log_level != "trace") {
        std::cerr << "error: --log-level must be info|debug|trace\n";
        return kConfigError;
    }
    if (positional.size() == 1) {
        if (positional[0] == "-") {
            if (!read_stdin(&question)) {
                std::cerr << "error: stdin is empty\n";
                return kRuntimeError;
            }
        } else {
            question = positional[0];
        }
    } else {
        std::cerr << "usage: mini-agent ask [--tools off|ask|allowlist|auto] [--cwd PATH] "
                     "\"question\" | mini-agent ask - (stdin)\n";
        return kConfigError;
    }
    // v5 CLI interrupt: TTY only (read question above may already have
    // consumed a piped stdin; non-TTY keeps the default SIGINT == the
    // historical byte-identical script behavior).
    // v5 CLI interrupt: install_cli_sigint is TTY-gated, so on fully
    // piped runs the flag can never be set and cli_abort_requested()
    // stays false -- the request path is behaviorally unchanged.
    install_cli_sigint();

    mini_agent::Logger logger(
        mini_agent::Logger::resolve_path(config).value_or(std::filesystem::path{}));
    mini_agent::OpenAiProvider provider(provider_with_retry_log(config, logger));
    if (config.migrated_from_v1)
        logger.event("config_migrate", {{"from", "1"}, {"to", "2"},
                                        {"profiles", std::to_string(config.providers.size())}});
    mini_agent::Chat chat;

    // Tool wiring for --tools ask|auto (008 5.4). off (default) keeps the
    // byte-identical v1 request/behavior. ask in a non-TTY downgrades to
    // auto with a stderr note: a one-shot CLI cannot block on prompts
    // portably; use the TUI for interactive approval.
    std::unique_ptr<mini_agent::ToolRegistry> registry;
    mini_agent::TurnOptions turn_options;
    if (tools_mode != "off") {
        // v3: validate the workspace boundary before any tool can run.
        if (!config.workspace.empty()) {
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(config.workspace, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                std::cerr << "error: workspace is not a directory: " << config.workspace << '\n';
                return kConfigError;
            }
            config.workspace = canonical.string();
        }
        std::string note;
        registry = mini_agent::build_tool_registry(config, &note);
        turn_options.tools = registry.get();
        turn_options.max_tool_rounds = config.max_tool_rounds;
        if (tools_mode == "ask") {
            if (::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO)) {
                turn_options.approve = [&registry](const mini_agent::ToolCall& call) {
                    auto parsed = nlohmann::json::parse(call.arguments_json, nullptr, false);
                    std::string primary;
                    if (parsed.is_object()) {
                        for (const char* key : {"command", "path", "text", "url", "query"}) {
                            auto it = parsed.find(key);
                            if (it != parsed.end() && it->is_string()) {
                                primary = it->get<std::string>();
                                break;
                            }
                        }
                    }
                    if (primary.empty())
                        primary = call.arguments_json;
                    // Prompt on STDERR: stdout stays reply-only (the ask
                    // pipe contract); the TTY still shows both streams.
                    // v3: file writes / commits render their diff preview
                    // first when the runtime provides one (same hook as
                    // the TUI approval block).
                    const auto preview = registry ? registry->preview(call) : std::string();
                    if (!preview.empty()) {
                        std::cerr << "\n[approve] " << call.name << " -- review before "
                                  << "allowing:\n"
                                  << preview;
                    } else {
                        std::cerr << "\n[approve] " << call.name << ": " << primary << '\n';
                    }
                    std::cerr << "allow? [y/N] " << std::flush;
                    std::string answer;
                    if (!std::getline(std::cin, answer)) {
                        if (registry)
                            registry->disarm(call);
                        return mini_agent::Approval::Deny;
                    }
                    const bool allowed = !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
                    if (!allowed && registry)
                        registry->disarm(call);
                    return allowed ? mini_agent::Approval::Allow : mini_agent::Approval::Deny;
                };
            } else {
                // Design 008 section 5.4 + ADR 006: non-interactive ask
                // degrades to tools-off (deterministic script contract),
                // NEVER to auto: silently running run_bash inside a pipe
                // would hand the shell to the model with no human in the
                // loop.
                std::cerr << "note: --tools ask needs a TTY; running with tools off\n";
                tools_mode = "off";
                turn_options.tools = nullptr;
                turn_options.approve = {};
            }
        } else if (tools_mode == "allowlist") {
            // v3 (design 009 section 2 CLI tier): deterministic AND
            // script-safe -- only names in config tool_allowlist execute;
            // everything else is denied as an error Tool message. No TTY
            // needed: nothing ever blocks.
            auto allowlist = config.tool_allowlist;
            turn_options.approve = [allowlist](const mini_agent::ToolCall& call) {
                return std::find(allowlist.begin(), allowlist.end(), call.name) != allowlist.end()
                           ? mini_agent::Approval::Allow
                           : mini_agent::Approval::Deny;
            };
        }
        turn_options.on_tool = [&, registry_ptr = registry.get()](
                                   const mini_agent::ToolCall& call,
                                   const mini_agent::ToolResult& result, bool executed) {
            // Metadata only (008 section 6): name/status/ms/bytes, never
            // arguments or output text.
            logger.event("tool_exec", {{"tool", call.name},
                                       {"executed", executed ? "yes" : "no"},
                                       {"status", result.is_error ? "error" : "ok"},
                                       {"elapsed_ms", std::to_string(result.elapsed_ms)},
                                       {"bytes", std::to_string(result.text.size())}});
            std::cerr << "[tool] " << call.name << " " << (result.is_error ? "error" : "ok")
                      << "\n";
            // v5 (design 011): on a TTY, echo the applied diff / preview
            // body to stderr so `ask --tools ask|allowlist` shows the same
            // edit -> diff flow as the TUI. stdout keeps the reply-only
            // pipe contract; non-TTY runs stay byte-identical to v3.
            if (executed && !result.is_error && ::isatty(STDERR_FILENO) == 1) {
                const auto body = registry_ptr->result_preview(call, result);
                if (!body.empty())
                    std::cerr << body << (body.back() == '\n' ? "" : "\n");
            }
        };
        logger.event("tools", {{"mode", "ask_cli"}, {"level", tools_mode}});
    }

    const auto started = std::chrono::steady_clock::now();
    try {
        // stdout receives ONLY the reply so it pipes cleanly; deltas go to
        // stdout as they arrive (single writer, no interleaving risk).
        // Tool status lines print to stderr, keeping the stdout contract.
        mini_agent::run_turn(
            chat, provider, question,
            [](std::string_view delta) { std::cout << delta << std::flush; },
            cli_abort_requested,
            turn_options);
        std::cout << '\n';
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        logger.event("turn",
                     {{"mode", "ask"},
                      {"status", std::string(mini_agent::Logger::turn_status_name(
                                     mini_agent::Logger::TurnStatus::Ok))},
                      {"elapsed_ms", std::to_string(ms)},
                      {"chars_out", std::to_string(chat.messages().back().content.size())}});
    } catch (const mini_agent::ProviderError& error) {
        print_provider_error(error);
        logger.event("turn", {{"mode", "ask"},
                              {"status", "error"},
                              {"kind", std::to_string(static_cast<int>(error.kind()))},
                              {"error", error.what()}});
        return kRuntimeError;
    }
    return kOk;
}

// v1 line-mode interaction. The self-drawn TUI (ADR 003) replaces this in
// v1.x behind the same run/session plumbing; until then `run` is usable.
int cmd_run(int argc, char** argv) {
    std::string session_id;
    bool fresh = false;
    bool temp = false; // v4.15: ephemeral session (never saved, removed on exit)
    std::string workspace_flag;
    bool debug_flag = false; // v4.4: any capture-raising flag was given
    std::string debug_level; // v4.8: its level (info|debug|trace)
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        } else if (std::strcmp(argv[i], "--new") == 0) {
            fresh = true;
        } else if (std::strcmp(argv[i], "--temp") == 0) {
            temp = true; // v4.15: implies --new; the chat dies with the process
        } else if (std::strcmp(argv[i], "--mcp") == 0 && i + 1 < argc) {
            ++i; // consumed by apply_mcp_flags below
        } else if (std::strncmp(argv[i], "--mcp=", 6) == 0) {
            // consumed by apply_mcp_flags below
        } else if ((std::strcmp(argv[i], "--provider") == 0 ||
                    std::strcmp(argv[i], "--model") == 0) &&
                   i + 1 < argc) {
            ++i; // consumed by apply_provider_flags below
        } else if (std::strncmp(argv[i], "--provider=", 11) == 0 ||
                   std::strncmp(argv[i], "--model=", 8) == 0) {
            // consumed by apply_provider_flags below
        } else if (std::strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            workspace_flag = argv[++i]; // v3: session workspace override
        } else if (std::strncmp(argv[i], "--cwd=", 6) == 0) {
            workspace_flag = argv[i] + 6;
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            debug_flag = true; // v4.4 alias: raises capture to debug
            debug_level = "debug";
        } else if (std::strcmp(argv[i], "--debug=all") == 0) {
            debug_flag = true; // v4.6 alias: raises capture to trace
            debug_level = "trace";
        } else if (std::strncmp(argv[i], "--log-level=", 12) == 0) {
            debug_flag = true; // v4.8 (design 010 §19)
            debug_level = argv[i] + 12;
        } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            debug_flag = true; // v4.8 (design 010 §19)
            debug_level = argv[++i];
        } else {
            std::cerr << "usage: mini-agent run [--session ID|PREFIX] [--new] [--temp] "
                         "[--cwd PATH] [--mcp NAME=SPEC] [--provider NAME] [--model NAME] "
                         "[--debug|=all] [--log-level info|debug|trace]\n";
            return kConfigError;
        }
    }
    if (temp) {
        // v4.15: --temp always opens a new chat; reuse never applies. A
        // --session ref would contradict "discarded on exit", so reject.
        if (!session_id.empty()) {
            std::cerr << "error: --temp cannot be combined with --session\n";
            return kConfigError;
        }
        fresh = true;
    }
    mini_agent::AppConfig config;
    std::string config_error;
    const bool configured = load_app_config(&config, &config_error);
    std::string provider_flag_error;
    if (!apply_provider_flags(argc, argv, &config, &provider_flag_error)) {
        std::cerr << "error: " << provider_flag_error << '\n';
        return kConfigError;
    }
    if (!workspace_flag.empty())
        config.workspace = workspace_flag; // CLI flag beats file/env (v3 009 5.4)
    if (debug_flag) {
        // v4.8: CLI flag beats file/env; unknown levels fail closed.
        if (debug_level != "info" && debug_level != "debug" && debug_level != "trace") {
            std::cerr << "error: --log-level must be info|debug|trace\n";
            return kConfigError;
        }
        config.log_level = debug_level;
    }
    std::string mcp_error;
    if (!apply_mcp_flags(argc, argv, &config, &mcp_error)) {
        std::cerr << "error: " << mcp_error << '\n';
        return kConfigError;
    }
    const bool tty = ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
    if (!configured && !config.complete() && !tty) {
        std::cerr << config_error << '\n'; // non-TTY cannot /login
        return kConfigError;
    }

    auto store = make_store();
    std::string io_error;
    if (!store.ensure_root(&io_error)) {
        std::cerr << "error: " << io_error << '\n';
        return kRuntimeError;
    }

    mini_agent::Session session;
    bool reused = false;
    // v4.15 (maintainer request): bare `run` reuses the most recently
    // updated session for this workspace instead of opening a fresh chat on
    // every launch. Candidates come from list_by_recency (save refreshes
    // mtime); the first one whose workspace stamp matches and whose lock is
    // free wins. A legacy unstamped session matched only when this launch
    // has no workspace either (v3 inherit rule, design 009 5.4). No match,
    // no session, or all busy -> fresh session like before. --new bypasses
    // reuse; --temp forces fresh AND never saves.
    if (session_id.empty() && !fresh) {
        std::error_code ec;
        const std::string launch_cwd =
            std::filesystem::weakly_canonical(std::filesystem::current_path(), ec).string();
        std::string want = config.workspace.empty() ? launch_cwd : config.workspace;
        want = std::filesystem::weakly_canonical(want, ec).string();
        std::string list_error;
        std::size_t tried = 0;
        for (const auto& id : store.list_by_recency(&list_error)) {
            if (++tried > 12)
                break; // cap the scan: recent sessions are the point here
            std::string load_error;
            auto candidate = store.load(id, &load_error);
            if (!candidate)
                continue; // unreadable or corrupt: older ones may still match
            std::string stamped = candidate->meta.workspace.value_or(std::string{});
            if (stamped.empty()) {
                if (!config.workspace.empty())
                    continue; // unstamped inherits the old launch cwd, not ours
                stamped = launch_cwd;
            } else {
                std::error_code wec;
                stamped = std::filesystem::weakly_canonical(stamped, wec).string();
            }
            if (stamped != want)
                continue;
            std::string lock_error;
            if (!store.try_lock(id, &lock_error))
                continue; // open in another process: try the next candidate
            session = std::move(*candidate);
            reused = true;
            // v3 workspace precedence on resume (same rule as --session):
            // the session's own stamp rebinds the boundary.
            if (session.meta.workspace && !session.meta.workspace->empty())
                config.workspace = *session.meta.workspace;
            break;
        }
    }
    if (!reused && (session_id.empty() || fresh)) {
        session.meta.id = mini_agent::generate_session_id();
        session.meta.model = config.current.model;
        // v3 (design 009 5.4/5.5): resolve + validate the workspace now so
        // a bad --cwd fails before the lock, and stamp it into the session.
        if (!config.workspace.empty()) {
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(config.workspace, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                std::cerr << "error: workspace is not a directory: " << config.workspace << '\n';
                return kConfigError;
            }
            config.workspace = canonical.string();
        }
        session.meta.workspace = config.workspace.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>(config.workspace);
        // v6 (design 012 §5.4): record which profile the model belongs to
        // (absent = env-only route); history only, never re-applied on load.
        session.meta.provider = config.current.name.empty()
                                    ? std::optional<std::string>{}
                                    : std::optional<std::string>(config.current.name);
        mini_agent::inject_system_prompt(session.chat, config.workspace, config.current.model,
                                         config.system_prompt);
    } else if (!reused) {
        if (!mini_agent::session_id_is_safe(session_id)) {
            std::cerr << "error: unsafe session id\n";
            return kConfigError;
        }
        // v4.13: --session accepts a unique prefix too. Ambiguous input
        // lists the candidates and exits; no match falls through to load
        // for the usual not-found error.
        {
            std::string list_error;
            const auto ids = store.list(&list_error);
            const auto m = mini_agent::match_session_ref(session_id, ids);
            if (m.exact) {
                session_id = m.id;
            } else if (!m.candidates.empty()) {
                std::cerr << "error: session id '" << session_id << "' is ambiguous:\n";
                for (const auto& id : m.candidates)
                    std::cerr << "  " << id << '\n';
                return kConfigError;
            }
        }
        auto loaded = store.load(session_id, &io_error);
        if (!loaded) {
            std::cerr << "error: " << io_error << '\n';
            return kRuntimeError;
        }
        session = std::move(*loaded);
        // v3 workspace precedence on resume: an explicit --cwd rebinds the
        // session (and is persisted); otherwise the session's own stamp
        // wins; otherwise config/env/launch cwd (009 5.4).
        if (!workspace_flag.empty()) {
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(config.workspace, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                std::cerr << "error: workspace is not a directory: " << config.workspace << '\n';
                return kConfigError;
            }
            config.workspace = canonical.string();
            session.meta.workspace = config.workspace;
        } else if (session.meta.workspace && !session.meta.workspace->empty()) {
            config.workspace = *session.meta.workspace;
        }
    }
    if (!reused && !store.try_lock(session.meta.id, &io_error)) {
        // `reused` already holds the lock taken during candidate scan.
        std::cerr << "error: " << io_error << '\n';
        return kRuntimeError;
    }

    if (tty) {
        mini_agent::tui::TuiOptions tui_options;
        tui_options.model = config.current.model;
        tui_options.session_id = session.meta.id;
        tui_options.resumed = reused || !session_id.empty(); // v4.15: banner tag
        tui_options.temp = temp;
        const int code = mini_agent::tui::run_loop(config, session, store, tui_options);
        store.unlock(session.meta.id);
        return code;
    }
    // Non-TTY fallback: line-mode (keeps scripted E2E valid; design 007 §9).
    // v5 CLI interrupt: the TUI branch above owns SIGINT itself (raw mode +
    // SIG_IGN); line mode installs the cooperative Ctrl-C handler here.
    install_cli_sigint();
    mini_agent::Logger logger(
        mini_agent::Logger::resolve_path(config).value_or(std::filesystem::path{}));
    mini_agent::OpenAiProvider provider(provider_with_retry_log(config, logger));
    if (config.migrated_from_v1)
        logger.event("config_migrate", {{"from", "1"}, {"to", "2"},
                                        {"profiles", std::to_string(config.providers.size())}});
    logger.event("session_open",
                 {{"mode", "line"}, {"id", session.meta.id}, {"model", config.current.model}});
    std::cout << "session " << session.meta.id << " (model " << config.current.model << ")\n"
              << "type a line to chat; /exit or EOF (Ctrl-D) exits\n";
    std::string line;
    for (;;) {
        // Fresh cycle: drop any sticky flag left by the previous (possibly
        // cancelled) turn, so each turn takes at most its own Ctrl-C(s).
        // During a turn the abort stays sticky: every should_abort query
        // sees it until run_turn unwinds. Two Ctrl-C within one turn =
        // hard kill (handler re-raises); Ctrl-C at idle only discards the
        // partial line. Exit stays /exit or EOF (Ctrl-D) as before.
        reset_cli_sigint();
        std::cout.flush();
        if (!std::getline(std::cin, line)) {
            // Without SA_RESTART a first Ctrl-C lands here as EINTR:
            // discard the (partial) line and keep the session alive --
            // the same "first Ctrl-C interrupts, does not quit" semantic
            // as the TUI. The flag (set by the handler) distinguishes an
            // interrupt from real EOF, so we do not depend on errno
            // surviving the iostream layers.
            if (g_sigint_count.load(std::memory_order_relaxed) == 1) {
                std::cin.clear();
                continue;
            }
            break;
        }
        if (line.empty())
            continue;
        if (line == "/exit")
            break; // same save/unlock path as EOF (design 007 5.6)
        const auto started = std::chrono::steady_clock::now();
        try {
            mini_agent::run_turn(session.chat, provider, line,
                                 [](std::string_view delta) { std::cout << delta << std::flush; },
                                 cli_abort_requested);
            std::cout << '\n';
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
            logger.event("turn", {{"mode", "line"},
                                  {"status", "ok"},
                                  {"elapsed_ms", std::to_string(ms)},
                                  {"chars_out",
                                   std::to_string(session.chat.messages().back().content.size())}});
            if (temp)
                continue; // v4.15: --temp line-mode never touches the store
            if (!store.save(session, &io_error)) {
                std::cerr << "warn: save failed: " << io_error << '\n';
                logger.event("session_save", {{"status", "error"}, {"error", io_error}});
            }
        } catch (const mini_agent::ProviderError& error) {
            print_provider_error(error);
            logger.event("turn", {{"mode", "line"},
                                  {"status", "error"},
                                  {"kind", std::to_string(static_cast<int>(error.kind()))},
                                  {"error", error.what()}});
        }
    }
    if (temp) {
        std::cout << "\ntemp session " << session.meta.id << " discarded (not saved)\n";
        logger.event("session_close",
                     {{"mode", "line"}, {"id", session.meta.id}, {"temp", "discarded"}});
    } else {
        std::cout << "\nsaved session " << session.meta.id << '\n';
        logger.event("session_close", {{"mode", "line"}, {"id", session.meta.id}});
    }
    store.unlock(session.meta.id);
    return kOk;
}

int cmd_session(int argc, char** argv) {
    auto store = make_store();
    std::string io_error;
    if (!store.ensure_root(&io_error)) {
        std::cerr << "error: " << io_error << '\n';
        return kRuntimeError;
    }
    if (argc == 3 && std::strcmp(argv[2], "list") == 0) {
        for (const auto& id : store.list(&io_error)) {
            std::cout << id << '\n';
        }
        if (!io_error.empty()) {
            std::cerr << "error: " << io_error << '\n';
            return kRuntimeError;
        }
        return kOk;
    }
    if (argc == 4 && std::strcmp(argv[2], "rm") == 0) {
        // v4.14.1: same prefix resolution as the TUI's /session -- an
        // unambiguous prefix removes the hit, an ambiguous one lists the
        // candidates instead of silently failing on a nonexistent id.
        const std::string ref = argv[3];
        std::string list_err;
        const auto ids = store.list(&list_err);
        const auto m = mini_agent::match_session_ref(ref, ids);
        if (m.exact) {
            if (!store.remove(m.id, &io_error)) {
                std::cerr << "error: " << io_error << '\n';
                return kRuntimeError;
            }
            std::cout << "removed session " << m.id << '\n';
            return kOk;
        }
        if (!m.candidates.empty()) {
            std::cerr << "error: ambiguous session prefix '" << ref << "':\n";
            for (const auto& id : m.candidates)
                std::cerr << "  " << id << '\n';
            return kConfigError;
        }
        if (!store.remove(ref, &io_error)) { // exact-file path keeps the "no such session" wording
            std::cerr << "error: " << io_error << '\n';
            return kRuntimeError;
        }
        std::cout << "removed session " << ref << '\n';
        return kOk;
    }
    std::cerr << "usage: mini-agent session list | mini-agent session rm ID\n"
                 "  (ID may be a full session id or an unambiguous prefix)\n";
    return kConfigError;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--help") == 0) {
        print_usage(std::cout); // no env, no network: cold-start budget
        return kOk;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << MINI_AGENT_VERSION << '\n';
        return kOk;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int code = kConfigError;
    if (argc >= 2 && std::strcmp(argv[1], "ask") == 0) {
        code = cmd_ask(argc, argv);
    } else if (argc >= 2 && std::strcmp(argv[1], "run") == 0) {
        code = cmd_run(argc, argv);
    } else if (argc >= 2 && std::strcmp(argv[1], "session") == 0) {
        code = cmd_session(argc, argv);
    } else if (argc >= 2 && std::strcmp(argv[1], "mcp") == 0) {
        code = cmd_mcp(argc, argv);
    } else if (argc == 1) {
        // v4.9: bare `mini-agent` opens the TUI directly (== `run`), so a
        // session switch is one command away: /session <id> inside.
        char implicit[] = "run";
        char* run_argv[] = {argv[0], implicit};
        code = cmd_run(2, run_argv);
    } else {
        std::cerr << "error: unknown command: " << argv[1] << "\n\n";
        print_usage(std::cerr);
        code = kConfigError;
    }
    curl_global_cleanup();
    return code;
}
