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

#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"
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
           "  mini-agent ask \"question\"      one-shot question to stdout\n"
           "  mini-agent ask -               read question from stdin\n"
           "  mini-agent run [--session ID]  interactive session (TUI; v1.0 fallback line mode)\n"
           "  mini-agent session list|rm ID  manage sessions\n"
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

int cmd_ask(int argc, char** argv) {
    mini_agent::AppConfig config;
    std::string config_error;
    if (!load_app_config(&config, &config_error)) {
        std::cerr << config_error << '\n';
        return kConfigError;
    }
    std::string question;
    if (argc == 3 && std::strcmp(argv[2], "-") == 0) {
        if (!read_stdin(&question)) {
            std::cerr << "error: stdin is empty\n";
            return kRuntimeError;
        }
    } else if (argc == 3) {
        question = argv[2];
    } else {
        std::cerr << "usage: mini-agent ask \"question\" | mini-agent ask - (stdin)\n";
        return kConfigError;
    }

    mini_agent::OpenAiProvider provider(config.to_provider());
    mini_agent::Logger logger(
        mini_agent::Logger::resolve_path(config).value_or(std::filesystem::path{}));
    mini_agent::Chat chat;
    const auto started = std::chrono::steady_clock::now();
    try {
        // stdout receives ONLY the reply so it pipes cleanly; deltas go to
        // stdout as they arrive (single writer, no interleaving risk).
        mini_agent::run_turn(chat, provider, question,
                             [](std::string_view delta) { std::cout << delta << std::flush; });
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
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        } else if (std::strcmp(argv[i], "--new") == 0) {
            fresh = true;
        } else {
            std::cerr << "usage: mini-agent run [--session ID] [--new]\n";
            return kConfigError;
        }
    }
    mini_agent::AppConfig config;
    std::string config_error;
    const bool configured = load_app_config(&config, &config_error);
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
    } else {
        print_usage(std::cerr);
        code = kConfigError;
    }
    curl_global_cleanup();
    return code;
}
