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

namespace {

bool host_is_loopback(std::string_view host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1" ||
           host.rfind("127.", 0) == 0;
}

// Extract scheme + host from an http(s) URL (config-validation level only;
// not a general parser).
bool split_url(std::string_view url, std::string* scheme, std::string* host) {
    const auto sep = url.find("://");
    if (sep == std::string_view::npos)
        return false;
    *scheme = std::string(url.substr(0, sep));
    auto rest = url.substr(sep + 3);
    const auto slash = rest.find('/');
    if (slash != std::string_view::npos)
        rest = rest.substr(0, slash);
    const auto at = rest.rfind('@');
    if (at != std::string_view::npos)
        rest = rest.substr(at + 1); // strip userinfo for host compare
    const auto colon = rest.find(':');
    *host =
        colon == std::string_view::npos ? std::string(rest) : std::string(rest.substr(0, colon));
    return !host->empty();
}

} // namespace

bool parse_mcp_cli_spec(std::string_view flag_value, McpServerConfig* out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error)
            *error = m;
        return false;
    };
    const auto eq = flag_value.find('=');
    if (eq == std::string_view::npos || eq == 0)
        return fail("--mcp expects NAME=SPEC (name=... uses '=' first)");
    McpServerConfig server;
    server.name = std::string(flag_value.substr(0, eq));
    const std::string spec(flag_value.substr(eq + 1));
    if (server.name.size() > 32)
        return fail("--mcp name too long (max 32)");
    for (char c : server.name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return fail("--mcp name must match [A-Za-z0-9_-]+");
    }
    if (spec.empty())
        return fail("--mcp spec is empty");

    if (spec.front() == '{') {
        // JSON entry form: identical schema to config.json's mcp_servers.
        auto parsed = nlohmann::json::parse(spec, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
            return fail("--mcp json spec must be a JSON object");
        AppConfig probe;
        nlohmann::json wrapped;
        wrapped[server.name] = std::move(parsed);
        // Reuse the file loader path for parsing + validation in one go:
        // config_from_json owns the field rules (single source of truth).
        const std::string text = wrapped.dump();
        // Build a minimal file object and let the same code parse it.
        nlohmann::json file;
        file["mcp_servers"] = nlohmann::json::parse(text);
        std::string json_error;
        auto loaded = config_from_json(file.dump(), &json_error);
        if (!loaded || loaded->mcp_servers.empty()) {
            return fail(json_error.empty() ? "--mcp json spec rejected" : "--mcp: " + json_error);
        }
        *out = loaded->mcp_servers.front();
        out->name = server.name; // JSON form may not override the CLI name
        return true;
    }

    if (spec.rfind("http://", 0) == 0 || spec.rfind("https://", 0) == 0) {
        server.transport = "http";
        server.url = spec;
    } else {
        // stdio shorthand: command + whitespace-split args
        server.transport = "stdio";
        std::istringstream stream(spec);
        stream >> server.command;
        std::string arg;
        while (stream >> arg)
            server.args.push_back(arg);
        if (server.command.empty())
            return fail("--mcp stdio form needs a command");
    }
    std::string valid_error;
    if (!server.valid(&valid_error))
        return fail("--mcp " + valid_error);
    *out = std::move(server);
    return true;
}

void upsert_mcp_server(AppConfig* config, const McpServerConfig& server) {
    for (auto& existing : config->mcp_servers) {
        if (existing.name == server.name) {
            existing = server; // CLI overrides file config (env-like priority)
            return;
        }
    }
    config->mcp_servers.push_back(server);
}

bool McpServerConfig::valid(std::string* error) const {
    auto fail = [&](const std::string& m) {
        if (error)
            *error = name.empty() ? m : ("mcp server '" + name + "': " + m);
        return false;
    };
    if (name.empty())
        return fail("server entry missing name");
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return fail("server name must match [A-Za-z0-9_-]+ (used in tool names)");
    }
    if (transport == "stdio") {
        if (command.empty())
            return fail("stdio transport requires 'command'");
        return true;
    }
    if (transport == "http") {
        if (url.empty())
            return fail("http transport requires 'url'");
        std::string scheme, host;
        if (!split_url(url, &scheme, &host))
            return fail("url must be absolute with http/https scheme");
        if (scheme == "https")
            return true;
        if (scheme != "http")
            return fail("unsupported url scheme");
        if (!host_is_loopback(host) && !allow_insecure_http)
            return fail("plain http allowed only for loopback unless allow_insecure_http");
        return true;
    }
    return fail("unknown transport '" + transport + "' (stdio|http)");
}

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
    // v3 fields (design 009 section 5.4): optional, written only when set.
    if (!config.workspace.empty())
        root["workspace"] = config.workspace;
    if (config.system_prompt.has_value()) {
        // "" (explicit disable) is written; absent = built-in default.
        root["system_prompt"] = *config.system_prompt;
    }
    // v2 fields (design 008 section 5.3): written only when they differ
    // from defaults, keeping v1-era files compact and diff-clean.
    if (config.tool_approval != "ask")
        root["tool_approval"] = config.tool_approval;
    if (!config.tool_allowlist.empty())
        root["tool_allowlist"] = config.tool_allowlist;
    if (config.max_tool_rounds != 8)
        root["max_tool_rounds"] = config.max_tool_rounds;
    if (config.tool_timeout_seconds != 30)
        root["tool_timeout_seconds"] = config.tool_timeout_seconds;
    if (!config.mcp_servers.empty()) {
        root["mcp_servers"] = nlohmann::json::object();
        for (const auto& server : config.mcp_servers) {
            nlohmann::json entry;
            entry["transport"] = server.transport;
            if (server.transport == "stdio") {
                entry["command"] = server.command;
                if (!server.args.empty())
                    entry["args"] = server.args;
                if (!server.env.empty()) {
                    entry["env"] = nlohmann::json::object();
                    for (const auto& kv : server.env)
                        entry["env"][kv.first] = kv.second;
                }
            } else {
                entry["url"] = server.url;
                if (!server.bearer_token.empty())
                    entry["bearer_token"] = server.bearer_token;
                if (server.allow_insecure_http)
                    entry["allow_insecure_http"] = true;
            }
            root["mcp_servers"][server.name] = std::move(entry);
        }
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

    // v3 optional fields.
    config.workspace = get_str("workspace");
    if (auto it = parsed.find("system_prompt"); it != parsed.end()) {
        if (it->is_string()) {
            config.system_prompt = it->get<std::string>();
        } else if (!it->is_null()) {
            if (error)
                *error = "config system_prompt must be a string or null";
            return std::nullopt;
        }
    }

    // v2 optional fields; unknown/absent -> defaults (forward-read rule).
    if (auto approval = get_str("tool_approval"); !approval.empty()) {
        if (approval != "ask" && approval != "allowlist" && approval != "off") {
            if (error)
                *error = "config tool_approval must be ask|allowlist|off";
            return std::nullopt;
        }
        config.tool_approval = approval;
    }
    if (auto it = parsed.find("tool_allowlist"); it != parsed.end()) {
        if (!it->is_array()) {
            if (error)
                *error = "config tool_allowlist must be an array";
            return std::nullopt;
        }
        for (const auto& entry : *it) {
            if (!entry.is_string()) {
                if (error)
                    *error = "config tool_allowlist entries must be strings";
                return std::nullopt;
            }
            config.tool_allowlist.push_back(entry.get<std::string>());
        }
    }
    auto get_size = [&](const char* key, std::size_t fallback) -> std::size_t {
        if (auto it = parsed.find(key);
            it != parsed.end() && it->is_number_integer() && it->get<int64_t>() > 0)
            return static_cast<std::size_t>(it->get<int64_t>());
        return fallback;
    };
    config.max_tool_rounds = get_size("max_tool_rounds", config.max_tool_rounds);
    config.tool_timeout_seconds = get_size("tool_timeout_seconds", config.tool_timeout_seconds);

    if (auto servers = parsed.find("mcp_servers");
        servers != parsed.end() && servers->is_object()) {
        for (auto& [name, node] : servers->items()) {
            if (!node.is_object()) {
                if (error)
                    *error = "mcp server '" + name + "' is not an object";
                return std::nullopt;
            }
            McpServerConfig server;
            server.name = name;
            if (auto it = node.find("transport"); it != node.end() && it->is_string()) {
                server.transport = it->get<std::string>();
            } else if (node.find("url") != node.end()) {
                server.transport = "http"; // inference (008 5.3)
            }
            // `name` is a structured binding: capture its reference target
            // explicitly instead of the binding itself (C++20 extension
            // under -Wpedantic).
            const std::string server_name = name;
            const nlohmann::json& entry = node; // named ref: capturing the
            // binding itself in a lambda is a C++20 extension
            auto sget = [&entry](const char* key) -> std::string {
                if (auto it = entry.find(key); it != entry.end() && it->is_string())
                    return it->get<std::string>();
                return {};
            };
            server.name = server_name;
            server.command = sget("command");
            server.url = sget("url");
            server.bearer_token = sget("bearer_token");
            if (auto it = node.find("allow_insecure_http"); it != node.end() && it->is_boolean())
                server.allow_insecure_http = it->get<bool>();
            if (auto it = node.find("args"); it != node.end() && it->is_array()) {
                for (const auto& arg : *it) {
                    if (!arg.is_string()) {
                        if (error)
                            *error = "mcp server '" + name + "' args must be strings";
                        return std::nullopt;
                    }
                    server.args.push_back(arg.get<std::string>());
                }
            }
            if (auto it = node.find("env"); it != node.end() && it->is_object()) {
                for (auto& [key, value] : it->items()) {
                    if (!value.is_string()) {
                        if (error)
                            *error = "mcp server '" + name + "' env values must be strings";
                        return std::nullopt;
                    }
                    server.env.emplace_back(key, value.get<std::string>());
                }
            }
            std::string valid_error;
            if (!server.valid(&valid_error)) {
                if (error)
                    *error = valid_error;
                return std::nullopt;
            }
            config.mcp_servers.push_back(std::move(server));
        }
    }
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
    if (const char* v = env_nonempty("MINI_AGENT_WORKSPACE"))
        config.workspace = v; // v3: scripts pin the boundary per invocation
    if (const char* v = env_nonempty("MINI_AGENT_TOOL_APPROVAL"); v && *v) {
        const std::string approval = v;
        if (approval == "ask" || approval == "allowlist" || approval == "off")
            config.tool_approval = approval; // invalid values keep file config
    }
    if (env_nonempty("MINI_AGENT_NO_MCP"))
        config.mcp_servers.clear(); // tri-state debug switch (008 5.3)
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
