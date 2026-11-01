#include "mini_agent/config.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

namespace mini_agent {
namespace {

const char* env_nonempty(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

bool ends_with_slash(const std::string& s) {
    return !s.empty() && s.back() == '/';
}

} // namespace

std::vector<std::string> AppConfig::missing() const {
    std::vector<std::string> out;
    if (base_url.empty())
        out.push_back("base_url");
    if (api_key.empty())
        out.push_back("api_key");
    if (model.empty())
        out.push_back("model");
    return out;
}

ProviderConfig AppConfig::to_provider() const {
    ProviderConfig p;
    p.base_url = base_url;
    p.api_key = api_key;
    p.model = model;
    if (const char* timeout = env_nonempty("MINI_AGENT_TIMEOUT_SECONDS")) {
        const long parsed = std::strtol(timeout, nullptr, 10);
        if (parsed > 0)
            p.total_timeout_seconds = static_cast<std::size_t>(parsed);
    }
    return p;
}

std::filesystem::path default_config_path() {
    if (const char* direct = env_nonempty("MINI_AGENT_CONFIG")) {
        return direct;
    }
    if (const char* xdg = env_nonempty("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "mini-agent" / "config.json";
    }
    if (const char* home = env_nonempty("HOME")) {
        return std::filesystem::path(home) / ".config" / "mini-agent" / "config.json";
    }
    return {};
}

std::string config_to_json(const AppConfig& config) {
    nlohmann::json root;
    root["schema_version"] = config.schema_version;
    root["base_url"] = config.base_url;
    root["api_key"] = config.api_key;
    root["model"] = config.model;
    if (!config.log_file.empty()) {
        root["log_file"] = config.log_file;
    }
    return root.dump(2);
}

std::optional<AppConfig> config_from_json(std::string_view text, std::string* error) {
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "config file is not a JSON object";
        return std::nullopt;
    }
    auto version_it = parsed.find("schema_version");
    if (version_it != parsed.end() && version_it->is_number_integer() &&
        version_it->get<int>() > 1) {
        if (error)
            *error = "config schema_version newer than supported (max 1)";
        return std::nullopt;
    }
    AppConfig config;
    auto get_str = [&parsed](const char* key) {
        if (auto it = parsed.find(key); it != parsed.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return std::string();
    };
    config.base_url = get_str("base_url");
    config.api_key = get_str("api_key");
    config.model = get_str("model");
    config.log_file = get_str("log_file");
    return config;
}

AppConfig load_config_merged(std::string* file_error) {
    AppConfig config;
    const auto path = default_config_path();
    if (!path.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            std::ifstream stream(path, std::ios::binary);
            std::ostringstream buffer;
            if (stream) {
                buffer << stream.rdbuf();
                const auto text = buffer.str();
                if (text.size() > static_cast<std::size_t>(64) * 1024u) {
                    if (file_error)
                        *file_error = "config file too large; ignoring";
                } else if (auto loaded = config_from_json(text, file_error)) {
                    config = std::move(*loaded);
                }
            }
        }
    }
    // env overrides file (non-empty only)
    if (const char* v = env_nonempty("MINI_AGENT_BASE_URL"))
        config.base_url = v;
    if (const char* v = env_nonempty("MINI_AGENT_API_KEY"))
        config.api_key = v;
    if (const char* v = env_nonempty("MINI_AGENT_MODEL"))
        config.model = v;
    if (const char* v = env_nonempty("MINI_AGENT_LOG"))
        config.log_file = v;
    if (!config.base_url.empty() && ends_with_slash(config.base_url)) {
        config.base_url.pop_back(); // tolerate trailing slash in file
    }
    return config;
}

bool save_config_file(const AppConfig& config, std::string* error) {
    const auto path = default_config_path();
    if (path.empty()) {
        if (error)
            *error = "no config path: set MINI_AGENT_CONFIG or HOME";
        return false;
    }
    std::error_code ec;
    const auto dir = path.parent_path();
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error)
            *error = "cannot create config dir: " + ec.message();
        return false;
    }
    if (::chmod(dir.c_str(), 0700) != 0) {
        if (error)
            *error = "cannot chmod 0700 config dir";
        return false;
    }
    const std::string tmp = path.string() + ".tmp";
    {
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            if (error)
                *error = "cannot open config temp file";
            return false;
        }
        const std::string payload = config_to_json(config);
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        const bool ok = written == static_cast<ssize_t>(payload.size());
        if (ok)
            ::fsync(fd);
        ::close(fd);
        if (!ok) {
            ::unlink(tmp.c_str());
            if (error)
                *error = "short write on config temp file";
            return false;
        }
        if (::chmod(tmp.c_str(), 0600) != 0) {
            ::unlink(tmp.c_str());
            if (error)
                *error = "cannot chmod 0600 config temp file";
            return false;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        if (error)
            *error = "config rename failed";
        return false;
    }
    return true;
}

} // namespace mini_agent
