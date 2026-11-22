#include "mini_agent/mcp.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
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

// M2a (design 022 §5.1): kind carried by a notifications/<kind>/list_changed
// line, empty for anything else. Shared by both backends' observers.
std::string_view list_changed_kind(std::string_view method) {
    if (method == "notifications/tools/list_changed")
        return "tools";
    if (method == "notifications/resources/list_changed")
        return "resources";
    if (method == "notifications/prompts/list_changed")
        return "prompts";
    return {};
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
        // design 008 section 5.2). Exception: the three list_changed
        // notifications feed the M2a observer (design 022 §5.1).
        auto id_it = reply.find("id");
        if (id_it == reply.end() || !id_it->is_number_integer() || id_it->get<long long>() != id) {
            if (list_changed_cb_ && id_it == reply.end()) {
                if (auto m = reply.find("method"); m != reply.end() && m->is_string()) {
                    const auto kind = list_changed_kind(m->get_ref<const std::string&>());
                    if (!kind.empty())
                        list_changed_cb_(kind);
                }
            }
            continue;
        }
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

void McpRpcClient::set_protocol_version(const std::string& version) {
    set_http_transport_version(*transport_, version);
}

void McpRpcClient::set_list_changed_callback(std::function<void(std::string_view)> cb) {
    list_changed_cb_ = std::move(cb);
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
        // Backend switch (design 021 §5.1). An empty factory means
        // production routing: cxxmcp is the default since the M1 gates
        // went green (2026-11-22); MINI_AGENT_MCP_BACKEND=legacy is the
        // rollback escape hatch until legacy removal in M3. Any other
        // value falls back to the cxxmcp default. An injected factory
        // (test fakes) always takes the legacy path and the switch is
        // bypassed. The switch must run BEFORE any factory call -- the
        // default factory always yields a transport, so consulting it
        // first would make the cxxmcp branch unreachable (M1 phase B
        // erratum, fixed in phase C).
        const char* backend = std::getenv("MINI_AGENT_MCP_BACKEND");
        const bool use_legacy =
            backend != nullptr && std::string_view(backend) == "legacy";
        if (!factory_ && !use_legacy) {
            rpc_ = make_cxxmcp_session(config_, handshake_timeout_, call_timeout_);
        } else {
            auto transport = factory_ ? factory_(config_) : default_transport_factory()(config_);
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
        // M2a observer (design 022 §5.1): list_changed notifications only
        // set atomic flags; caches re-fetch on the next owner-thread list.
        // May fire from a transport thread (cxxmcp) or inline while the
        // owner blocks in recv (legacy) -- atomic-only is safe for both.
        rpc_->set_list_changed_callback([this](std::string_view kind) {
            if (kind == "tools")
                tools_stale_.store(true, std::memory_order_relaxed);
            else if (kind == "resources")
                resources_stale_.store(true, std::memory_order_relaxed);
            else if (kind == "prompts")
                prompts_stale_.store(true, std::memory_order_relaxed);
        });
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
    // Read-only capabilities client: no roots/sampling/elicitation offered
    // (M2b adds the server-request bridge, design 022 §5.2).
    params["capabilities"] = nlohmann::json::object();
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
    rpc_->set_protocol_version(negotiated_version_);
    if (auto caps = parsed.find("capabilities"); caps != parsed.end() && caps->is_object()) {
        tools_capability_ = caps->find("tools") != caps->end();
        resources_capability_ = caps->find("resources") != caps->end();
        prompts_capability_ = caps->find("prompts") != caps->end();
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
    // Detach the observer BEFORE closing: a transport thread may still be
    // draining notifications while stop() runs, and the callback captures
    // `this` (design 022 §5.1 shutdown order).
    if (rpc_) {
        rpc_->set_list_changed_callback({});
        rpc_->close();
    }
    rpc_.reset();
    down_ = false;
    initialized_ = false;
    tools_capability_ = false;
    resources_capability_ = false;
    prompts_capability_ = false;
    negotiated_version_.clear();
    last_error_.clear();
    cache_valid_ = false;
    tool_cache_.clear();
    resources_cache_.clear();
    prompts_cache_.clear();
    tools_stale_.store(false, std::memory_order_relaxed);
    resources_stale_.store(false, std::memory_order_relaxed);
    prompts_stale_.store(false, std::memory_order_relaxed);
}

std::vector<ToolDefinition> McpClient::list() {
    std::vector<ToolDefinition> out;
    if (down_)
        return out;
    // M2a: a list_changed notification invalidated the tool cache; the
    // next listing re-walks pages (design 022 §5.1). Owner thread only.
    if (tools_stale_.exchange(false, std::memory_order_relaxed))
        cache_valid_ = false;
    // Hot path: with a valid cache we NEVER touch the transport. run_turn
    // asks the registry for tools every provider round; without this short
    // circuit a multi-round turn would pay one extra tools/list round-trip
    // per round (found in the 2026-11-05 implementation review).
    if (!cache_valid_) {
        std::string error;
        if (!ensure_started(&error))
            return out;
        if (tools_capability_) {
            tool_cache_.clear();
            // Pagination: follow nextCursor until empty (spec Tools listing).
            std::string cursor;
            for (int page = 0; page < 16; ++page) {
                nlohmann::json params = nlohmann::json::object();
                if (!cursor.empty())
                    params["cursor"] = cursor;
                std::string result;
                if (!rpc_->request("tools/list", params.dump(), handshake_timeout_, &result,
                                   &error)) {
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
    }
    for (const auto& info : tool_cache_) {
        ToolDefinition definition;
        definition.name = cache_wire_name(info);
        definition.description = info.description;
        definition.input_schema_json = info.input_schema_json;
        definition.source = config_.name;
        out.push_back(std::move(definition));
    }
    // M2a synthetic tools (design 022 §5.1): the resource/prompt surface
    // rides the ordinary ToolRuntime path, so ADR 006 approval, previews
    // and loop truncation apply unchanged. Gated on the capability probes.
    const auto append_synthetic = [&](const char* suffix, std::string description,
                                      const char* schema) {
        ToolDefinition definition;
        definition.name = config_.name + "__" + suffix;
        definition.description = std::move(description);
        definition.input_schema_json = schema;
        definition.source = config_.name;
        out.push_back(std::move(definition));
    };
    if (resources_capability_)
        append_synthetic(
            "mcp_resources",
            "List or read the resources advertised by MCP server '" + config_.name +
                "'. action=list returns resource metadata; action=read requires a "
                "uri from that listing and returns its contents.",
            R"json({"type":"object","properties":{"action":{"type":"string","enum":["list","read"]},"uri":{"type":"string","description":"resource URI (action=read)"}},"required":["action"]})json"
        );
    if (prompts_capability_)
        append_synthetic(
            "mcp_prompts",
            "List or render the prompt templates advertised by MCP server '" + config_.name +
                "'. action=list returns prompt metadata; action=get requires a "
                "name from that listing plus optional string arguments.",
            R"json({"type":"object","properties":{"action":{"type":"string","enum":["list","get"]},"name":{"type":"string","description":"prompt name (action=get)"},"arguments":{"type":"object","description":"prompt arguments as string values","additionalProperties":{"type":"string"}}},"required":["action"]})json"
        );
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

    // M2a synthetic tools never reach the remote tools/call path: they
    // dispatch to the resource/prompt seam locally (design 022 §5.1).
    if (call.name == config_.name + "__mcp_resources")
        return call_synthetic(call, /*resources=*/true);
    if (call.name == config_.name + "__mcp_prompts")
        return call_synthetic(call, /*resources=*/false);

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
// M2a resource/prompt seam (design 022 §5.1)
// ---------------------------------------------------------------------------

std::string McpClient::list_paged(std::string_view method, std::string_view item_key,
                                  std::string* cache, std::atomic<bool>& stale,
                                  std::string* error) {
    if (!ensure_started(error))
        return {};
    const bool capable = method == "resources/list" ? resources_capability_ : prompts_capability_;
    if (!capable) {
        if (error)
            *error = "server does not support " + std::string(item_key);
        return {};
    }
    if (!stale.exchange(false, std::memory_order_relaxed) && !cache->empty())
        return *cache; // metadata cache hit (contents are never cached, T2)
    nlohmann::json all = nlohmann::json::array();
    std::string cursor;
    for (int page = 0; page < 16; ++page) { // same bounded walk as tools/list
        nlohmann::json params = nlohmann::json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;
        std::string result;
        if (!rpc_->request(method, params.dump(), handshake_timeout_, &result, error)) {
            down_ = true;
            if (error)
                last_error_ = *error;
            cache->clear();
            return {};
        }
        auto parsed = nlohmann::json::parse(result, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            down_ = true;
            last_error_ = std::string(method) + " result is not an object";
            if (error)
                *error = last_error_;
            cache->clear();
            return {};
        }
        auto items = parsed.find(std::string(item_key));
        if (items == parsed.end() || !items->is_array()) {
            down_ = true;
            last_error_ = std::string(method) + " missing " + std::string(item_key) + " array";
            if (error)
                *error = last_error_;
            cache->clear();
            return {};
        }
        for (const auto& entry : *items)
            all.push_back(entry);
        cursor.clear();
        if (auto nc = parsed.find("nextCursor"); nc != parsed.end() && nc->is_string())
            cursor = nc->get<std::string>();
        if (cursor.empty())
            break;
    }
    *cache = all.dump();
    return *cache;
}

std::string McpClient::list_resources(std::string* error) {
    return list_paged("resources/list", "resources", &resources_cache_, resources_stale_, error);
}

std::string McpClient::list_prompts(std::string* error) {
    return list_paged("prompts/list", "prompts", &prompts_cache_, prompts_stale_, error);
}

std::string McpClient::read_resource(const std::string& uri, std::string* error) {
    if (!ensure_started(error))
        return {};
    if (!resources_capability_) {
        if (error)
            *error = "server does not support resources";
        return {};
    }
    if (uri.empty()) {
        if (error)
            *error = "read_resource requires a uri";
        return {};
    }
    nlohmann::json params;
    params["uri"] = uri;
    std::string result;
    if (!rpc_->request("resources/read", params.dump(), call_timeout_, &result, error)) {
        // Same rule as tools/call: only transport death or timeout marks
        // down; a JSON-RPC error reply (unknown uri) keeps the server
        // usable -- the stream is still in sync.
        if (!rpc_->alive() || (error && error->find("timed out") != std::string::npos)) {
            down_ = true;
            if (error)
                last_error_ = *error;
        }
        return {};
    }
    auto parsed = nlohmann::json::parse(result, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "resources/read result is not an object";
        return {};
    }
    auto contents = parsed.find("contents");
    if (contents == parsed.end() || !contents->is_array()) {
        if (error)
            *error = "resources/read missing contents array";
        return {};
    }
    return contents->dump(); // raw contents; never cached (design 022 T2)
}

std::string McpClient::get_prompt(const std::string& name, const std::string& args_json,
                                  std::string* error) {
    if (!ensure_started(error))
        return {};
    if (!prompts_capability_) {
        if (error)
            *error = "server does not support prompts";
        return {};
    }
    if (name.empty()) {
        if (error)
            *error = "get_prompt requires a name";
        return {};
    }
    nlohmann::json arguments = nlohmann::json::object();
    if (!args_json.empty() && args_json != "{}") {
        arguments = nlohmann::json::parse(args_json, nullptr, false);
        if (arguments.is_discarded() || !arguments.is_object()) {
            if (error)
                *error = "prompt arguments must be a JSON object";
            return {};
        }
    }
    nlohmann::json params;
    params["name"] = name;
    params["arguments"] = std::move(arguments);
    std::string result;
    if (!rpc_->request("prompts/get", params.dump(), call_timeout_, &result, error)) {
        if (!rpc_->alive() || (error && error->find("timed out") != std::string::npos)) {
            down_ = true;
            if (error)
                last_error_ = *error;
        }
        return {};
    }
    auto parsed = nlohmann::json::parse(result, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "prompts/get result is not an object";
        return {};
    }
    // MRTR (SEP-2322) is only reachable at >= 2025-11-25, above the current
    // ceiling; fold it as an error rather than looping (design 022 §5.1).
    if (auto rt = parsed.find("resultType"); rt != parsed.end() && rt->is_string() &&
        rt->get_ref<const std::string&>() == "input_required") {
        if (error)
            *error = "prompt requires additional input (multi-round prompts are not supported)";
        return {};
    }
    return result; // raw prompts/get result; never cached
}

ToolResult McpClient::call_synthetic(const ToolCall& call, bool resources) {
    auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object())
        return ToolResult{"arguments must be a JSON object", true};
    const auto action_it = args.find("action");
    if (action_it == args.end() || !action_it->is_string())
        return ToolResult{resources ? "mcp_resources requires action=list|read"
                                    : "mcp_prompts requires action=list|get",
                          true};
    const std::string action = action_it->get<std::string>();
    std::string error;
    std::string text;
    if (action == "list") {
        text = resources ? list_resources(&error) : list_prompts(&error);
        if (!error.empty())
            return ToolResult{"mcp " + std::string(resources ? "resources" : "prompts") +
                                  " list failed: " + error,
                              true};
        return ToolResult{text.empty() ? "[]" : std::move(text), false};
    }
    if (resources && action == "read") {
        const auto uri_it = args.find("uri");
        if (uri_it == args.end() || !uri_it->is_string())
            return ToolResult{"action=read requires a uri string", true};
        text = read_resource(uri_it->get<std::string>(), &error);
        if (!error.empty())
            return ToolResult{"mcp resource read failed: " + error, true};
        return ToolResult{std::move(text), false};
    }
    if (!resources && action == "get") {
        const auto name_it = args.find("name");
        if (name_it == args.end() || !name_it->is_string())
            return ToolResult{"action=get requires a name string", true};
        std::string args_json = "{}";
        if (auto a = args.find("arguments"); a != args.end() && a->is_object())
            args_json = a->dump();
        text = get_prompt(name_it->get<std::string>(), args_json, &error);
        if (!error.empty())
            return ToolResult{"mcp prompt get failed: " + error, true};
        return ToolResult{std::move(text), false};
    }
    return ToolResult{resources ? "unknown action (expected list|read)"
                                : "unknown action (expected list|get)",
                      true};
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
        for (const auto& server : config.mcp_servers) {
            // Empty factory = production routing: MINI_AGENT_MCP_BACKEND
            // selects the backend (design 021 §5.1), default cxxmcp;
            // "legacy" is the explicit rollback value until M3.
            registry->add(std::make_unique<McpClient>(server, TransportFactory{}));
        }
        if (note)
            *note = std::to_string(config.mcp_servers.size()) + " mcp server(s) configured";
    }
    return registry;
}

} // namespace mini_agent
