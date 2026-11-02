#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mini_agent/config.hpp"

namespace mini_agent {

// Diagnostics file, ON BY DEFAULT at /tmp/mini-agent-<uid>.log (design 007
// section 7; default path added 2026-11-01 by maintainer request). uid
// suffix + 0600 keep the shared /tmp safe from cross-user collisions.
// Overrides: MINI_AGENT_LOG=<path>, config.log_file, or the literal "off"
// in either to disable (no-op sinks). Built on vendored spdlog.
//
// Hard guarantees (tested): events carry ONLY metadata — timestamps,
// model/base_url host, message/char counts, durations, status and error
// kinds. NEVER the API key, message bodies, or full prompts.
class Logger {
  public:
    // Resolution order: env MINI_AGENT_LOG > config.log_file >
    // /tmp/mini-agent-<uid>.log. The value "off" (either source) disables.
    static std::optional<std::string> resolve_path(const AppConfig& config);

    explicit Logger(const std::filesystem::path& path);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool enabled() const noexcept {
        return sink_ != nullptr;
    }

    // Releases spdlog's global registry (process exit path only).
    static void shutdown_all() noexcept;

    // Generic structured event: `event` plus key/value metadata pairs.
    void event(std::string_view name,
               const std::vector<std::pair<std::string, std::string>>& fields = {});

    // Redact URL userinfo before it is logged: curl accepts
    // https://user:token@host/v1, and such a URL in a log line would break
    // the "no secrets in logs" guarantee (design 007 section 6) even though
    // the api_key field itself is never logged. Scheme and host are kept for
    // diagnosability; anything between them and the first '/' is replaced by
    // `***@`. URLs without userinfo are returned unchanged.
    static std::string redact_url(std::string_view url);

    // Turn outcome vocabulary shared by CLI/TUI/line-mode.
    enum class TurnStatus { Ok, Error, Aborted };
    static std::string_view turn_status_name(TurnStatus status) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> sink_;
};

} // namespace mini_agent
