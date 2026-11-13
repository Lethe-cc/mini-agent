#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/provider.hpp"

namespace mini_agent {

// One MCP server entry (design 008 section 5.3). `transport` is "stdio"
// (spawn `command` with `args`, extra `env`) or "http" (Streamable HTTP at
// `url`, optional static `bearer_token`). Loaded config is inert until
// first use: servers start lazily (section 5.5).
struct McpServerConfig {
    std::string name;
    std::string transport = "stdio"; // "stdio" | "http"
    std::string command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env; // explicit only
    std::string url;
    std::string bearer_token; // never logged or echoed (like api_key)
    bool allow_insecure_http = false;

    bool valid(std::string* error) const;
};

// One named OpenAI-compatible provider profile (design 012 section 5.1).
// Names follow the MCP rule ([A-Za-z0-9_-]+, max 32) because they appear
// in menus and log lines. `model` remembers the last model selected for
// this profile (empty = never selected). A freshly added profile may
// still await its URL/key: valid() only checks non-empty values, so
// partial profiles are legal until first use.
struct ProviderProfile {
    std::string name;
    std::string base_url;
    std::string api_key; // never log or echo
    std::string model;   // last selected model for this profile
    long long context_window = 0;
    std::string reasoning_effort;

    bool valid(std::string* error) const;
};

// Runtime LLM configuration. Priority: environment variables override the
// config file (so CI/scripts keep working), and the TUI /login flow writes
// the file (so interactive users never need env vars).
//
// The api_key is persisted in the config file with 0600 permission by user
// action (/login). This is a conscious trade-off recorded in design 007
// section 6: without it, per-process login would be impossible in TUI.
// The file path honors MINI_AGENT_CONFIG for tests and custom locations.
//
// v7 (design 013, ADR 009): the ONLY persisted LLM state is the named
// profile table `providers` plus `active_provider` (schema_version 2).
// Effective values live in `current`, derived one-way from the active
// profile + env overlay; there is no unnamed persisted state and no
// write-back sync.
struct AppConfig {
    int schema_version = 2;
    std::string log_file; // optional diagnostics path (design 007 §7)
    // v4.8 (design 010 §19): unified traffic-capture log level, replacing
    // the v4.4/v4.6 debug_log/debug_log_aux bools. "info" (default):
    // metadata-only Logger, zero raw payloads (the standing guarantee).
    // "debug": chat completions also dump raw request/SSE bodies to
    // "<log path>.debug" (0600). "trace": ALSO auxiliary traffic (GET
    // /models, context probe) with payloads truncated at 4KB. Legacy
    // spellings MINI_AGENT_DEBUG / --debug / --debug=all / /debug
    // on|all|off map onto the same ladder. v4.8.1: the level is
    // SESSION-SCOPED -- never written to the config file and ignored if
    // present there (legacy debug_log/debug_log_aux fields are dropped
    // on the next save); sources are env, CLI flags, and the TUI
    // switches only.
    std::string log_level = "info"; // "info" | "debug" | "trace"
    // Derived capture gates (single source of truth: log_level).
    bool capture_chat() const {
        return log_level == "debug" || log_level == "trace";
    }
    bool capture_aux() const {
        return log_level == "trace";
    }

    // v3 (design 009 section 5.4): workspace root for file/git tools and
    // run_bash cwd. Empty = process cwd at launch. Never logged in full
    // (basename only is allowed -- path components can be sensitive).
    std::string workspace;
    // nullopt = built-in default prompt; present-but-empty = disabled
    // (design 009 section 5.5).
    std::optional<std::string> system_prompt;

    // v2 tool layer (design 008 §5.3). Approval policy: "ask" (default,
    // human-in-the-loop per ADR 006), "allowlist", "off".
    std::string tool_approval = "ask";
    std::vector<std::string> tool_allowlist;
    std::size_t max_tool_rounds = 8;
    std::size_t tool_timeout_seconds = 30;
    std::vector<McpServerConfig> mcp_servers;

    // v7 (design 013): named provider profiles are the only persisted LLM
    // state. active_provider must name an existing profile whenever
    // providers is non-empty (enforced at load, fail-closed).
    std::vector<ProviderProfile> providers;
    std::string active_provider;
    // Runtime effective values = active profile + non-empty env overlay +
    // session_model, rebuilt by refresh_current(). NEVER persisted.
    // name is empty only in the no-profile/env-only state. Model metadata
    // (context_window/reasoning_effort) is config-authoritative per profile
    // (design 010); switching profiles overwrites, never merges.
    ProviderProfile current;
    // Session-scoped, never persisted: any of MINI_AGENT_BASE_URL/_API_KEY/
    // _MODEL is in effect, so profile switching cannot change the effective
    // endpoint until the vars are unset and the process restarts. The TUI
    // keeps listing profiles but annotates picks with the override.
    bool env_override_active = false;
    // CLI --model session overlay, applied last in refresh_current();
    // never written to a profile or to disk (design 013 §5.3).
    std::string session_model;
    // Set when config_from_json migrated a schema<=1 file; callers log one
    // config_migrate metadata event. Runtime only, never persisted.
    bool migrated_from_v1 = false;

    // Profile lookup by name; nullptr for an empty or unknown name (a
    // dangling active_provider is rejected at load).
    ProviderProfile* find_provider(std::string_view name);
    const ProviderProfile* find_provider(std::string_view name) const;
    // Switch the active profile and re-derive current. Empty/unknown names
    // are rejected: the unnamed state no longer exists (design 013).
    bool activate_provider(std::string_view name);
    // Rebuild current from the active profile (or the env-qualified profile
    // when MINI_AGENT_MODEL=provider/model names one) plus the non-empty
    // env overlay and session_model. Called at load, after activation, and
    // after every profile edit. Env is immutable within a process, so
    // re-reading getenv here has no side effects.
    void refresh_current();

    bool complete() const noexcept {
        return !current.base_url.empty() && !current.api_key.empty() && !current.model.empty();
    }
    // Names of missing fields, for user-facing hints.
    std::vector<std::string> missing() const;

    ProviderConfig to_provider() const;
};

// MINI_AGENT_CONFIG > $XDG_CONFIG_HOME/mini-agent/config.json >
// $HOME/.config/mini-agent/config.json. Empty path when unresolvable.
std::filesystem::path default_config_path();

// Parse one --mcp CLI spec of the form NAME=SPEC (design 008 section 5.3
// runtime injection; session-scoped, does not touch config.json):
//   NAME=https://host/path | http://127.0.0.1:port/mcp  -> Streamable HTTP
//   NAME={json}  -> full entry, same fields as a config.json mcp_servers
//                   object (transport|command|args|env|url|bearer_token|...)
//   NAME=command [arg ...]  -> stdio, whitespace-split (args must not
//                   contain spaces; use the {json} form when they do)
// NAME must match [A-Za-z0-9_-]+ (feeds tool names, 008 deviation D1).
// The entry then passes the same valid() gate as file config.
bool parse_mcp_cli_spec(std::string_view flag_value, McpServerConfig* out, std::string* error);

// Apply one parsed server to config: same name replaces, else appends.
void upsert_mcp_server(AppConfig* config, const McpServerConfig& server);

// Split a qualified model reference "provider/model" at the FIRST '/'
// (model ids themselves may contain '/', e.g. openrouter ids). Returns
// false when there is no separator or either side is empty. Callers must
// still check that the provider part names a configured profile; an
// unknown prefix means the whole reference is a literal model id.
bool split_qualified_model(std::string_view ref, std::string* provider, std::string* model);

// Loads file config (absent file is fine -> empty AppConfig), then applies
// env overrides (MINI_AGENT_BASE_URL / _API_KEY / _MODEL, non-empty only).
AppConfig load_config_merged(std::string* file_error);

// Atomic write, dir 0700, file 0600. Schema_version is embedded.
bool save_config_file(const AppConfig& config, std::string* error);

std::string config_to_json(const AppConfig& config);
std::optional<AppConfig> config_from_json(std::string_view text, std::string* error);

} // namespace mini_agent
