#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mini_agent/provider.hpp"

namespace mini_agent {

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

// Loads file config (absent file is fine -> empty AppConfig), then applies
// env overrides (MINI_AGENT_BASE_URL / _API_KEY / _MODEL, non-empty only).
AppConfig load_config_merged(std::string* file_error);

// Atomic write, dir 0700, file 0600. Schema_version is embedded.
bool save_config_file(const AppConfig& config, std::string* error);

std::string config_to_json(const AppConfig& config);
std::optional<AppConfig> config_from_json(std::string_view text, std::string* error);

} // namespace mini_agent
