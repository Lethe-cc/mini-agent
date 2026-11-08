#pragma once

#include <filesystem>
#include <optional>
#include <string>
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

// Runtime LLM configuration. Priority: environment variables override the
// config file (so CI/scripts keep working), and the TUI /login flow writes
// the file (so interactive users never need env vars).
//
// The api_key is persisted in the config file with 0600 permission by user
// action (/login). This is a conscious trade-off recorded in design 007
// section 6: without it, per-process login would be impossible in TUI.
// The file path honors MINI_AGENT_CONFIG for tests and custom locations.
struct AppConfig {
    int schema_version = 1;
    std::string base_url;
    std::string api_key; // never log or echo
    std::string model;
    std::string log_file; // optional diagnostics path (design 007 §7)
    // v4.4: opt-in raw request/response capture for debugging. OFF by
    // default; the metadata-only Logger guarantee holds unless the user
    // explicitly enables this. When on, chat completions dump their raw
    // request body and SSE payloads to "<log path>.debug" (0600).
    // Auxiliary requests (GET /models listing, context-window probe) are
    // NEVER captured -- only conversation traffic.
    bool debug_log = false;
    // v4.6: level-2 debug -- ALSO dump auxiliary traffic (listing, probe)
    // with long payloads truncated. Off unless MINI_AGENT_DEBUG=2 or
    // /debug all. Chat traffic dumping is governed by debug_log alone.
    bool debug_log_aux = false;

    // v3 (design 009 section 5.4): workspace root for file/git tools and
    // run_bash cwd. Empty = process cwd at launch. Never logged in full
    // (basename only is allowed -- path components can be sensitive).
    std::string workspace;
    // nullopt = built-in default prompt; present-but-empty = disabled
    // (design 009 section 5.5).
    std::optional<std::string> system_prompt;

    // v4 (design 010): model metadata the gateway cannot be trusted to
    // provide. Config is the authoritative source -- no probing, no
    // override by server replies.
    // context_window: divisor for the status-bar ctx% reading; 0 = unset
    // (status bar then shows token counts without a percentage).
    long long context_window = 0;
    // reasoning_effort: verbatim passthrough into the request body when
    // non-empty ("low"|"medium"|"high"|provider-specific); empty = field
    // omitted == pre-v4 byte-identical request.
    std::string reasoning_effort;

    // v2 tool layer (design 008 §5.3). Approval policy: "ask" (default,
    // human-in-the-loop per ADR 006), "allowlist", "off".
    std::string tool_approval = "ask";
    std::vector<std::string> tool_allowlist;
    std::size_t max_tool_rounds = 8;
    std::size_t tool_timeout_seconds = 30;
    std::vector<McpServerConfig> mcp_servers;

    bool complete() const noexcept {
        return !base_url.empty() && !api_key.empty() && !model.empty();
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

// Loads file config (absent file is fine -> empty AppConfig), then applies
// env overrides (MINI_AGENT_BASE_URL / _API_KEY / _MODEL, non-empty only).
AppConfig load_config_merged(std::string* file_error);

// Atomic write, dir 0700, file 0600. Schema_version is embedded.
bool save_config_file(const AppConfig& config, std::string* error);

std::string config_to_json(const AppConfig& config);
std::optional<AppConfig> config_from_json(std::string_view text, std::string* error);

} // namespace mini_agent
