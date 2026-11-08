// mini-agent CLI. Exit codes (design 007 section 5): 0 success,
// 1 runtime error (network/HTTP/protocol/io), 2 configuration error.
#include <chrono>
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

void print_usage(std::ostream& out) {
    out << "mini-agent " MINI_AGENT_VERSION
        << " - a mini AI agent\n"
           "usage:\n"
           "  mini-agent ask [--tools M] [--cwd PATH] [--debug] \"question\"  one-shot to stdout\n"
           "  mini-agent ask [--tools M] --cwd PATH -   read question from stdin\n"
           "      --tools off|ask|allowlist|auto  (default off: non-interactive contract)\n"
           "          allowlist: only tool_allowlist names execute (script-safe, v3)\n"
           "      --debug  dump raw chat request/response to <log path>.debug (v4.4)\n"
           "  mini-agent run [--session ID] [--cwd PATH] [--debug]  session (TUI; v1 line-mode)\n"
           "  mini-agent session list|rm ID  manage sessions\n"
           "  mini-agent mcp list            show configured MCP servers\n"
           "  --mcp NAME=SPEC (ask/run/mcp)  add one server this session:\n"
           "      NAME=https://host/mcp          Streamable HTTP\n"
           "      NAME=cmd arg1 arg2             stdio subprocess\n"
           "      NAME={json entry}              full control (env, bearer)\n"
           "  mini-agent --help | --version  no network, no key needed\n"
           "\nenvironment:\n"
           "  MINI_AGENT_BASE_URL  e.g. https://api.openai.com/v1\n"
           "  MINI_AGENT_API_KEY   never printed\n"
           "  MINI_AGENT_MODEL     model name\n";
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
    if (!load_app_config(&config, &config_error)) {
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
        if (std::strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            config.workspace = argv[++i]; // v3 workspace override (009 5.4)
            continue;
        }
        if (std::strncmp(argv[i], "--cwd=", 6) == 0) {
            config.workspace = argv[i] + 6;
            continue;
        }
        if (std::strcmp(argv[i], "--debug") == 0) {
            config.debug_log = true; // v4.4: raw chat traffic dump (010 §15)
            continue;
        }
        if (std::strcmp(argv[i], "--debug=all") == 0) {
            config.debug_log = true; // v4.6: level 2, aux traffic too
            config.debug_log_aux = true;
            continue;
        }
        positional.emplace_back(argv[i]);
    }
    if (tools_mode != "off" && tools_mode != "ask" && tools_mode != "auto" &&
        tools_mode != "allowlist") {
        std::cerr << "error: --tools must be off|ask|allowlist|auto\n";
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

    mini_agent::OpenAiProvider provider(config.to_provider());
    mini_agent::Logger logger(
        mini_agent::Logger::resolve_path(config).value_or(std::filesystem::path{}));
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
        turn_options.on_tool = [&logger](const mini_agent::ToolCall& call,
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
            [](std::string_view delta) { std::cout << delta << std::flush; }, [] { return false; },
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
    std::string workspace_flag;
    bool debug_flag = false;     // v4.4
    bool debug_aux_flag = false; // v4.6
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        } else if (std::strcmp(argv[i], "--new") == 0) {
            fresh = true;
        } else if (std::strcmp(argv[i], "--mcp") == 0 && i + 1 < argc) {
            ++i; // consumed by apply_mcp_flags below
        } else if (std::strncmp(argv[i], "--mcp=", 6) == 0) {
            // consumed by apply_mcp_flags below
        } else if (std::strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            workspace_flag = argv[++i]; // v3: session workspace override
        } else if (std::strncmp(argv[i], "--cwd=", 6) == 0) {
            workspace_flag = argv[i] + 6;
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            debug_flag = true; // v4.4: raw chat traffic dump (design 010 §15)
        } else if (std::strcmp(argv[i], "--debug=all") == 0) {
            debug_flag = true; // v4.6: level 2 -- also aux traffic (truncated)
            debug_aux_flag = true;
        } else {
            std::cerr << "usage: mini-agent run [--session ID] [--new] "
                         "[--cwd PATH] [--mcp NAME=SPEC] [--debug|=all]\n";
            return kConfigError;
        }
    }
    mini_agent::AppConfig config;
    std::string config_error;
    const bool configured = load_app_config(&config, &config_error);
    if (!workspace_flag.empty())
        config.workspace = workspace_flag; // CLI flag beats file/env (v3 009 5.4)
    if (debug_flag) {
        config.debug_log = true; // v4.4: CLI flag beats file/env
        config.debug_log_aux = debug_aux_flag;
    }
    std::string mcp_error;
    if (!apply_mcp_flags(argc, argv, &config, &mcp_error)) {
        std::cerr << "error: " << mcp_error << '\n';
        return kConfigError;
    }
    const bool tty = ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
    if (!configured && !tty) {
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
    if (session_id.empty() || fresh) {
        session.meta.id = mini_agent::generate_session_id();
        session.meta.model = config.model;
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
        mini_agent::inject_system_prompt(session.chat, config.workspace, config.model,
                                         config.system_prompt);
    } else {
        if (!mini_agent::session_id_is_safe(session_id)) {
            std::cerr << "error: unsafe session id\n";
            return kConfigError;
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
    if (!store.try_lock(session.meta.id, &io_error)) {
        std::cerr << "error: " << io_error << '\n';
        return kRuntimeError;
    }

    if (tty) {
        mini_agent::tui::TuiOptions tui_options;
        tui_options.model = config.model;
        tui_options.session_id = session.meta.id;
        const int code = mini_agent::tui::run_loop(config, session, store, tui_options);
        store.unlock(session.meta.id);
        return code;
    }
    // Non-TTY fallback: line-mode (keeps scripted E2E valid; design 007 §9).
    mini_agent::OpenAiProvider provider(config.to_provider());
    mini_agent::Logger logger(
        mini_agent::Logger::resolve_path(config).value_or(std::filesystem::path{}));
    logger.event("session_open",
                 {{"mode", "line"}, {"id", session.meta.id}, {"model", config.model}});
    std::cout << "session " << session.meta.id << " (model " << config.model << ")\n"
              << "type a line to chat; /exit or EOF (Ctrl-D) exits\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        if (line == "/exit")
            break; // same save/unlock path as EOF (design 007 5.6)
        const auto started = std::chrono::steady_clock::now();
        try {
            mini_agent::run_turn(session.chat, provider, line,
                                 [](std::string_view delta) { std::cout << delta << std::flush; });
            std::cout << '\n';
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
            logger.event("turn", {{"mode", "line"},
                                  {"status", "ok"},
                                  {"elapsed_ms", std::to_string(ms)},
                                  {"chars_out",
                                   std::to_string(session.chat.messages().back().content.size())}});
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
    std::cout << "\nsaved session " << session.meta.id << '\n';
    logger.event("session_close", {{"mode", "line"}, {"id", session.meta.id}});
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
        if (!store.remove(argv[3], &io_error)) {
            std::cerr << "error: " << io_error << '\n';
            return kRuntimeError;
        }
        return kOk;
    }
    std::cerr << "usage: mini-agent session list | mini-agent session rm ID\n";
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
    } else {
        print_usage(std::cerr);
        code = kConfigError;
    }
    curl_global_cleanup();
    return code;
}
