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
// Build metadata (CMake configure time); fallbacks keep non-CMake builds
// and source tarballs working.
#ifndef MINI_AGENT_COMMIT
#define MINI_AGENT_COMMIT "unknown"
#endif
#ifndef MINI_AGENT_BUILD_TIME
#define MINI_AGENT_BUILD_TIME "unknown"
#endif
#include "cli_common.hpp"

using namespace mini_agent::cli;

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--help") == 0) {
        print_usage(std::cout); // no env, no network: cold-start budget
        return kOk;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << MINI_AGENT_VERSION << '\n';
        return kOk;
    }
    if (argc >= 2 && std::strcmp(argv[1], "version") == 0) {
        // Detailed build identity; like --version, no env/network/key.
        // --version stays version-only for the script contract.
        std::cout << "mini-agent " MINI_AGENT_VERSION "\n"
                  << "commit " MINI_AGENT_COMMIT "\n"
                  << "built " MINI_AGENT_BUILD_TIME "\n";
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
