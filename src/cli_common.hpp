#pragma once
// Internal CLI contract (design 019 P2): shared helpers + subcommands,
// split out of the former single cli_main.cpp translation unit.
#include <iosfwd>
#include <string>

#include "mini_agent/config.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"

namespace mini_agent::cli {
inline constexpr int kOk = 0;
inline constexpr int kRuntimeError = 1;
inline constexpr int kConfigError = 2;
void install_cli_sigint();
bool cli_abort_requested();
void reset_cli_sigint();
bool cli_sigint_once();
void print_usage(std::ostream& out);
bool read_stdin(std::string* out);
bool load_app_config(AppConfig* out, std::string* config_error);
SessionStore make_store();
void print_provider_error(const ProviderError& error);
ProviderConfig provider_with_retry_log(const AppConfig& config, Logger& logger);
bool apply_mcp_flags(int argc, char** argv, AppConfig* config, std::string* error);
bool apply_provider_flags(int argc, char** argv, AppConfig* config, std::string* error);
int cmd_mcp(int argc, char** argv);
int cmd_ask(int argc, char** argv);
int cmd_run(int argc, char** argv);
int cmd_session(int argc, char** argv);
} // namespace mini_agent::cli
