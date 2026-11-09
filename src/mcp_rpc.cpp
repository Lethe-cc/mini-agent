#include "mini_agent/mcp.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>

namespace mini_agent {
namespace {

// Latest protocol version we advertise first; the server may answer with
// an older supported one and we accept anything in this list (version
// negotiation, spec 2025-06-18 Lifecycle).
constexpr const char* kPreferredVersion = "2025-06-18";
bool negotiable_version(std::string_view version) {
    return version == "2025-06-18" || version == "2025-03-26" || version == "2024-11-05";
}

std::string rpc_error_text(const nlohmann::json& reply) {
    const auto error = reply.find("error");
    if (error == reply.end() || !error->is_object())
        return "malformed JSON-RPC error";
    std::string message = "rpc error";
    if (auto m = error->find("message"); m != error->end() && m->is_string())
        message = m->get<std::string>();
    if (auto c = error->find("code"); c != error->end() && c->is_number_integer()) {
        message = "rpc error " + std::to_string(c->get<int64_t>()) + ": " + message;
    }
    return message;
}

} // namespace

// ---------------------------------------------------------------------------
// McpRpcClient
// ---------------------------------------------------------------------------

bool McpRpcClient::start(std::string* error) {
    return transport_->start(error);
}

bool McpRpcClient::alive() const noexcept {
    return transport_->alive();
}

void McpRpcClient::close() noexcept {
    transport_->close();
}

bool McpRpcClient::request(std::string_view method, const std::string& params_json,
                           std::chrono::milliseconds timeout, std::string* result_json,
                           std::string* error) {
    if (!transport_->alive()) {
        if (error)
            *error = "mcp transport is closed";
        return false;
    }
    const long long id = next_id_++;
    nlohmann::json message;
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["method"] = std::string(method);
    if (!params_json.empty() && params_json != "{}") {
        auto params = nlohmann::json::parse(params_json, nullptr, false);
        if (params.is_discarded()) {
            if (error)
                *error = "internal: params_json is not valid JSON";
            return false;
        }
        message["params"] = std::move(params);
    }

    std::string send_error;
    if (!transport_->send(message.dump(), &send_error)) {
        if (error)
            *error = send_error.empty() ? "send failed" : send_error;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            if (error)
                *error = "mcp request timed out (" + std::string(method) + ")";
            return false;
        }
        std::string recv_error;
        auto next =
            transport_->recv(std::min(remaining, std::chrono::milliseconds(200)), &recv_error);
        if (recv_error.empty() && !next.has_value())
            continue; // poll slice elapsed, deadline still open
        if (!next) {
            if (error)
                *error = recv_error;
            return false;
        }
        auto reply = nlohmann::json::parse(*next, nullptr, false);
        if (reply.is_discarded() || !reply.is_object()) {
            if (error)
                *error = "mcp server sent a non-JSON-RPC line";
            return false;
        }
        // Match by id; anything else is a server notification or a
        // server-initiated request (unsupported capability: ignore per
        // design 008 section 5.2).
        auto id_it = reply.find("id");
        if (id_it == reply.end() || !id_it->is_number_integer() || id_it->get<long long>() != id)
            continue;
        if (reply.contains("error")) {
            if (error)
                *error = rpc_error_text(reply);
            return false;
        }
        if (result_json) {
            auto result = reply.find("result");
            *result_json = (result != reply.end()) ? result->dump() : std::string("{}");
        }
        return true;
    }
}

bool McpRpcClient::notify(std::string_view method, const std::string& params_json,
                          std::string* error) {
    nlohmann::json message;
    message["jsonrpc"] = "2.0";
    message["method"] = std::string(method);
    if (!params_json.empty() && params_json != "{}") {
        auto params = nlohmann::json::parse(params_json, nullptr, false);
        if (!params.is_discarded())
            message["params"] = std::move(params);
    }
    return transport_->send(message.dump(), error);
}

// ---------------------------------------------------------------------------
// McpClient
// ---------------------------------------------------------------------------

McpClient::McpClient(McpServerConfig config, TransportFactory factory,
                     std::chrono::milliseconds handshake_timeout,
                     std::chrono::milliseconds call_timeout)
    : config_(std::move(config)), factory_(std::move(factory)),
      handshake_timeout_(handshake_timeout), call_timeout_(call_timeout) {
}

bool McpClient::ensure_started(std::string* error) {
    if (initialized_)
        return true;
    if (down_) {
        if (error)
            *error = last_error_.empty() ? "mcp server marked down" : last_error_;
        return false;
    }
    if (!rpc_) {
        auto transport = factory_(config_);
        if (!transport) {
            // Failed / exhausted factory (one-shot test fakes included):
            // stay down instead of wrapping a null transport.
            down_ = true;
            last_error_ = "mcp transport factory returned nothing";
            if (error)
                *error = last_error_;
            return false;
        }
        rpc_ = std::make_unique<McpRpcClient>(std::move(transport));
    }
    std::string start_error;
    if (!rpc_->start(&start_error)) {
        down_ = true;
        last_error_ = start_error;
        if (error)
            *error = start_error;
        return false;
    }
    if (!handshake(&start_error)) {
        rpc_->close();
        down_ = true;
        last_error_ = start_error;
        if (error)
            *error = start_error;
        return false;
    }
    initialized_ = true;
    return true;
}

bool McpClient::handshake(std::string* error) {
    nlohmann::json params;
    params["protocolVersion"] = kPreferredVersion;
    params["capabilities"] = nlohmann::json::object(); // tools-only client
    params["clientInfo"]["name"] = "mini-agent";
    params["clientInfo"]["version"] = "0.2";

    std::string result;
    if (!rpc_->request("initialize", params.dump(), handshake_timeout_, &result, error))
        return false;
    auto parsed = nlohmann::json::parse(result, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "initialize result is not a JSON object";
        return false;
    }
    negotiated_version_.clear();
    if (auto v = parsed.find("protocolVersion"); v != parsed.end() && v->is_string()) {
        negotiated_version_ = v->get<std::string>();
    }
    if (!negotiated_version_.empty() && !negotiable_version(negotiated_version_)) {
        // Spec: client SHOULD disconnect on an unsupported version.
        if (error)
            *error = "server wants protocol version " + negotiated_version_ +
                     " which this client does not implement";
        return false;
    }
    if (negotiated_version_.empty())
        negotiated_version_ = kPreferredVersion; // pre-negotiation servers
    set_http_transport_version(rpc_->transport(), negotiated_version_);
    if (auto caps = parsed.find("capabilities"); caps != parsed.end() && caps->is_object()) {
        tools_capability_ = caps->find("tools") != caps->end();
    }
    // notifications/initialized: mandatory client follow-up.
    std::string notify_error;
    if (!rpc_->notify("notifications/initialized", "{}", &notify_error)) {
        if (error)
            *error = notify_error;
        return false;
    }
    return true;
}

void McpClient::restart() noexcept {
    if (rpc_)
        rpc_->close();
    rpc_.reset();
    down_ = false;
    initialized_ = false;
    tools_capability_ = false;
    negotiated_version_.clear();
    last_error_.clear();
    cache_valid_ = false;
    tool_cache_.clear();
}

std::vector<ToolDefinition> McpClient::list() {
    std::vector<ToolDefinition> out;
    if (down_)
        return out;
    // Hot path: with a valid cache we NEVER touch the transport. run_turn
    // asks the registry for tools every provider round; without this short
    // circuit a multi-round turn would pay one extra tools/list round-trip
    // per round (found in the 2026-11-05 implementation review).
    if (cache_valid_) {
        for (const auto& info : tool_cache_) {
            ToolDefinition definition;
            definition.name = cache_wire_name(info);
            definition.description = info.description;
            definition.input_schema_json = info.input_schema_json;
            definition.source = config_.name;
            out.push_back(std::move(definition));
        }
        return out;
    }
    std::string error;
    if (!ensure_started(&error))
        return out;
    if (!tools_capability_)
        return out;
    {
        tool_cache_.clear();
        // Pagination: follow nextCursor until empty (spec Tools listing).
        std::string cursor;
        for (int page = 0; page < 16; ++page) {
            nlohmann::json params = nlohmann::json::object();
            if (!cursor.empty())
                params["cursor"] = cursor;
            std::string result;
            if (!rpc_->request("tools/list", params.dump(), handshake_timeout_, &result, &error)) {
                down_ = true;
                last_error_ = error;
                cache_valid_ = false;
                return out;
            }
            auto parsed = nlohmann::json::parse(result, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                down_ = true;
                last_error_ = "tools/list result is not an object";
                return out;
            }
            auto tools_it = parsed.find("tools");
            if (tools_it == parsed.end() || !tools_it->is_array()) {
                down_ = true;
                last_error_ = "tools/list missing tools array";
                return out;
            }
            for (const auto& entry : *tools_it) {
                if (!entry.is_object())
                    continue;
                McpToolInfo info;
                if (auto n = entry.find("name"); n != entry.end() && n->is_string())
                    info.name = n->get<std::string>();
                if (info.name.empty())
                    continue; // unnamed tool: skip (annotations untrusted anyway)
                if (auto d = entry.find("description"); d != entry.end() && d->is_string())
                    info.description = d->get<std::string>();
                if (auto s = entry.find("inputSchema"); s != entry.end() && s->is_object())
                    info.input_schema_json = s->dump();
                else
                    info.input_schema_json = R"({"type":"object"})";
                tool_cache_.push_back(std::move(info));
            }
            cursor.clear();
            if (auto nc = parsed.find("nextCursor"); nc != parsed.end() && nc->is_string())
                cursor = nc->get<std::string>();
            if (cursor.empty())
                break;
        }
        cache_valid_ = true;
    }
    for (const auto& info : tool_cache_) {
        ToolDefinition definition;
        definition.name = cache_wire_name(info);
        definition.description = info.description;
        definition.input_schema_json = info.input_schema_json;
        definition.source = config_.name;
        out.push_back(std::move(definition));
    }
    return out;
}

std::string McpClient::cache_wire_name(const McpToolInfo& info) {
    // Namespace separator must stay inside the OpenAI function-name
    // grammar ^[A-Za-z0-9_-]+$ (008 5.2): gateways reject or silently
    // drop '/'-containing names (found via deepwiki live test 2026-11-04).
    // Server-side names are sanitized the same way; the ORIGINAL name
    // goes out in tools/call, so remotes keep working untouched.
    std::string safe = info.name;
    for (auto& ch : safe) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-'))
            ch = '_';
    }
    const std::string wire = config_.name + "__" + safe;
    wire_to_remote_[wire] = info.name; // call() maps back to the original
    return wire;
}

ToolResult McpClient::call(const ToolCall& call, const ShouldAbort& should_abort) {
    const std::string prefix = config_.name + "__";
    if (call.name.compare(0, prefix.size(), prefix) != 0)
        return ToolResult{"tool does not belong to server " + config_.name, true};
    if (should_abort())
        return ToolResult{"aborted before mcp call", true};

    std::string remote_name = call.name.substr(prefix.size());
    auto mapped = wire_to_remote_.find(call.name);
    if (mapped != wire_to_remote_.end())
        remote_name = mapped->second; // sanitized on listing; servers see originals
    nlohmann::json params;
    params["name"] = remote_name;
    auto arguments = nlohmann::json::parse(call.arguments_json, nullptr, false);
    params["arguments"] = arguments.is_discarded() || !arguments.is_object()
                              ? nlohmann::json::object()
                              : std::move(arguments);

    // One transparent recovery: if the transport DIED (stdio child
    // crashed/killed, HTTP endpoint down) the request never completed, so
    // restart + re-handshake + retry once is safe. We detect death by
    // transport `alive()` (not error strings). A timeout is NOT retried --
    // the tool may have run with side effects -- but it still marks the
    // client down like before (the RPC stream can desync after a late
    // reply, design 008 section 5.2).
    bool retried = false;
    std::string result;
    std::string error;
    // A server already marked down is NOT respawned here: /tools restart
    // and registry rebuild stay the operator's explicit paths, and a broken
    // command would otherwise pay the handshake timeout on every call.
    // Recovery below only covers a session that was up and then died.
    for (;;) {
        error.clear();
        result.clear();
        bool dead = false;
        if (!ensure_started(&error)) {
            return ToolResult{"mcp server down: " + error, true};
        } else if (!rpc_->alive()) {
            error = "transport closed";
            dead = true;
        } else if (!rpc_->request("tools/call", params.dump(), call_timeout_, &result, &error)) {
            dead = !rpc_->alive(); // transport died mid-request
            if (!dead && error.find("timed out") != std::string::npos) {
                down_ = true; // stream desync risk: keep the v2.0 rule
                last_error_ = error;
            }
        }
        if (!result.empty() || !dead)
            break; // success, or a non-transport failure to report below
        if (retried) {
            down_ = true; // restart did not help: stop respawning per call
            last_error_ = error;
            break;
        }
        retried = true;
        restart(); // clears down_, drops rpc_: fresh transport + handshake
    }
    if (result.empty())
        return ToolResult{"mcp call failed: " + error, true};
    if (should_abort())
        return ToolResult{"aborted after mcp response", true};

    auto parsed = nlohmann::json::parse(result, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return ToolResult{"mcp result is not a JSON object", true};

    // Result content: concatenate text blocks (v2.0 ignores resource
    // links/images/structured output, design 008 section 2).
    ToolResult out;
    auto content_it = parsed.find("content");
    if (content_it != parsed.end() && content_it->is_array()) {
        for (const auto& block : *content_it) {
            if (!block.is_object())
                continue;
            auto type_it = block.find("type");
            if (type_it != block.end() && type_it->is_string() &&
                type_it->get_ref<const std::string&>() == "text") {
                if (auto text_it = block.find("text");
                    text_it != block.end() && text_it->is_string()) {
                    if (!out.text.empty())
                        out.text += "\n";
                    out.text += text_it->get<std::string>();
                }
            } else {
                out.text +=
                    "[" +
                    (type_it != block.end() && type_it->is_string() ? type_it->get<std::string>()
                                                                    : std::string("unknown")) +
                    " content elided]";
            }
        }
    }
    if (auto is_error = parsed.find("isError"); is_error != parsed.end() && is_error->is_boolean())
        out.is_error = is_error->get<bool>();
    if (out.text.empty())
        out.text = out.is_error ? "tool reported an error (no content)" : "(empty result)";
    return out;
}

// ---------------------------------------------------------------------------
// Factory routing
// ---------------------------------------------------------------------------

TransportFactory default_transport_factory() {
    return [](const McpServerConfig& config) -> std::unique_ptr<McpTransport> {
        if (config.transport == "http")
            return make_http_transport(config);
        return make_stdio_transport(config);
    };
}

} // namespace mini_agent

namespace mini_agent {

std::unique_ptr<ToolRegistry> build_tool_registry(const AppConfig& config, std::string* note) {
    // v3 file tools (design 009 5.1): workspace root defaults to the
    // process cwd when unset. Construction never fails.
    std::filesystem::path workspace = config.workspace.empty()
                                          ? std::filesystem::current_path()
                                          : std::filesystem::path(config.workspace);
    auto registry = std::make_unique<ToolRegistry>();
    registry->add(std::make_unique<BuiltinRuntime>(config.tool_timeout_seconds, workspace));
    FileToolsConfig file_config;
    file_config.workspace = workspace;
    registry->add(std::make_unique<FileRuntime>(std::move(file_config)));
    // v3 git tools (design 009 5.2): lazy probe on first use; absent
    // entirely when git is missing or the workspace is no repository.
    registry->add(std::make_unique<GitRuntime>(workspace, config.tool_timeout_seconds));
    if (!config.mcp_servers.empty()) {
        auto factory = default_transport_factory();
        for (const auto& server : config.mcp_servers) {
            registry->add(std::make_unique<McpClient>(server, factory));
        }
        if (note)
            *note = std::to_string(config.mcp_servers.size()) + " mcp server(s) configured";
    }
    return registry;
}

} // namespace mini_agent
