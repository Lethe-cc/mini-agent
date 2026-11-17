#include "mini_agent/log.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

// Header-only vendored spdlog (third_party/spdlog, MIT, v1.17.0).
#define SPDLOG_HEADER_ONLY
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace mini_agent {

struct Logger::Impl {
    std::shared_ptr<spdlog::logger> log;
};

std::optional<std::string> Logger::resolve_path(const AppConfig& config) {
    if (config.log_file == "off")
        return std::nullopt;
    if (!config.log_file.empty())
        return config.log_file;
    // Default: shared /tmp, uid-suffixed and 0600 (see header contract).
    return "/tmp/mini-agent-" + std::to_string(static_cast<long>(::getuid())) + ".log";
}

Logger::Logger(const std::filesystem::path& path) {
    std::error_code ec;
    if (!path.empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        sink_ = std::make_unique<Impl>();
        try {
            static int logger_serial = 0;
            const std::string name = "mini_agent_" + std::to_string(++logger_serial);
            sink_->log =
                spdlog::basic_logger_mt(name, path.string(), false /* append, never truncate */);
            sink_->log->set_pattern("%Y-%m-%dT%H:%M:%S%z %v");
            (void)::chmod(path.c_str(), 0600); // tighten after creation
        } catch (const spdlog::spdlog_ex&) {
            sink_.reset(); // unwritable path: logging stays disabled
        }
    }
}

Logger::~Logger() {
    if (sink_ && sink_->log)
        spdlog::drop(sink_->log->name());
}

void Logger::shutdown_all() noexcept {
    try {
        spdlog::shutdown();
    } catch (...) { // NOLINT(bugprone-empty-catch) noexcept boundary: nothing actionable at exit
    }
}

std::string_view Logger::turn_status_name(TurnStatus status) noexcept {
    switch (status) {
    case TurnStatus::Ok:
        return "ok";
    case TurnStatus::Error:
        return "error";
    case TurnStatus::Aborted:
        return "aborted";
    }
    return "unknown";
}

std::string Logger::redact_url(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos)
        return std::string(url); // not a URL we understand: keep as-is
    const std::size_t authority_begin = scheme + 3;
    // Authority ends at the first '/', '?' or '#'; anything before it may
    // carry `user:password@`.
    const auto authority_end = url.find_first_of("/?#", authority_begin);
    const auto authority = url.substr(authority_begin, authority_end - authority_begin);
    const auto at = authority.rfind('@');
    if (at == std::string_view::npos)
        return std::string(url); // no userinfo
    std::string out(url.substr(0, authority_begin));
    out += "***@";
    out.append(authority.substr(at + 1));
    if (authority_end != std::string_view::npos)
        out.append(url.substr(authority_end));
    return out;
}

void Logger::event(std::string_view name,
                   const std::vector<std::pair<std::string, std::string>>& fields) {
    if (!sink_ || !sink_->log)
        return;
    std::string payload(name);
    for (const auto& [key, value] : fields) {
        payload += " " + key + "=";
        payload += value;
    }
    sink_->log->info("{}", payload);
}

} // namespace mini_agent
