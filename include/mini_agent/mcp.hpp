#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/config.hpp"
#include "mini_agent/tools.hpp"

namespace mini_agent {

// v2 MCP client (design 008 section 5.2, ADR 005). Three layers:
//   McpTransport  -- byte-level message carrier (stdio, HTTP, future IPC)
//   McpRpcClient  -- JSON-RPC id correlation over one transport
//   McpClient     -- MCP dialogue (initialize/tools) as a ToolRuntime
// Baseline protocol 2025-06-18, fallback 2025-03-26 (verified online
// 2026-11-02). Tools surface as "server/tool" in the registry.

// One JSON-RPC message either way, as serialized text: the header stays
// nlohmann-free (same rule as chat.hpp). Implementations own framing --
// stdio appends '\n'; HTTP wraps requests and drains response queues.
class McpTransport {
  public:
    virtual ~McpTransport() = default;
    // Open the channel (spawn subprocess / verify endpoint reachability
    // lazily). Idempotent; false + *error on failure.
    virtual bool start(std::string* error) = 0;
    virtual bool alive() const = 0;
    // One complete JSON-RPC message out.
    virtual bool send(std::string_view json_message, std::string* error) = 0;
    // Next complete message in, or nullopt on timeout; error set only for
    // dead channels (a timeout returns nullopt with no error).
    virtual std::optional<std::string> recv(std::chrono::milliseconds timeout,
                                            std::string* error) = 0;
    virtual void close() noexcept = 0;
    // Server-side diagnostics (stdio stderr tail / last HTTP status text).
    // Metadata-safe: callers may log length only, never the content.
    virtual std::string diagnostics_tail() const {
        return {};
    }
};

using TransportFactory = std::function<std::unique_ptr<McpTransport>(const McpServerConfig&)>;

// JSON-RPC 2.0 client over one transport: monotonic ids, strictly serial
// in-flight requests (v2.0 design), foreign-id messages ignored (server
// notifications/requests are outside our capability set and folded away,
// design 008 section 5.2). Not a general pool -- one connection per server.
class McpRpcClient {
  public:
    explicit McpRpcClient(std::unique_ptr<McpTransport> transport)
        : transport_(std::move(transport)) {
    }

    bool start(std::string* error);
    bool alive() const noexcept;
    void close() noexcept;
    McpTransport& transport() noexcept {
        return *transport_;
    }

    // Sends `{jsonrpc,id,method,params}` and waits for the matching
    // response. On success *result_json carries the raw "result" node;
    // JSON-RPC "error" replies set *error (with code) and return false.
    bool request(std::string_view method, const std::string& params_json,
                 std::chrono::milliseconds timeout, std::string* result_json, std::string* error);
    // Fire-and-forget notification; transport failure is reported, a
    // server 202/absent reply is success by definition.
    bool notify(std::string_view method, const std::string& params_json, std::string* error);

  private:
    std::unique_ptr<McpTransport> transport_;
    long long next_id_ = 1;
};

// One discovered tool from tools/list.
struct McpToolInfo {
    std::string name;        // server-side name
    std::string description; // UNTRUSTED model-visible text
    std::string input_schema_json;
};

// MCP dialogue + registry adapter for a single server. Lazy by contract:
// `ensure_started()` runs transport start + initialize + initialized on
// first use (cold-start guarantee, design 008 section 5.5). After a hard
// failure the client marks itself down; `restart()` clears the state.
//
// Threading: NOT internally synchronized -- one client instance is owned
// by exactly one caller at a time (CLI: the turn; TUI: the worker between
// joins, main thread rebuilding only after join). list()/call()/restart()
// mutate the cache and transport; concurrent use is a contract violation.
class McpClient final : public ToolRuntime {
  public:
    McpClient(McpServerConfig config, TransportFactory factory,
              std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10),
              std::chrono::milliseconds call_timeout = std::chrono::seconds(120));

    // ToolRuntime interface. list() returns empty while down or when the
    // server lacks the tools capability; names are namespaced
    // "<server>/<tool>" so one registry stays unambiguous.
    std::vector<ToolDefinition> list() override;
    ToolResult call(const ToolCall& call, const ShouldAbort& should_abort) override;
    bool alive() const override {
        return !down_;
    }

    // Start + handshake; idempotent. False keeps the client usable but
    // not initialized; repeated failures mark it down.
    bool ensure_started(std::string* error);
    void restart() noexcept;
    const std::string& server_name() const noexcept {
        return config_.name;
    }
    // Negotiated protocol version after handshake (empty before).
    const std::string& protocol_version() const noexcept {
        return negotiated_version_;
    }
    bool supports_tools() const noexcept {
        return tools_capability_;
    }
    // Last startup/handshake error text for UI display (never contains
    // bearer tokens: the transport layer composes these messages).
    const std::string& last_error() const noexcept {
        return last_error_;
    }

  private:
    bool handshake(std::string* error);
    // Registry-visible wire name for a cached server tool: namespaced with
    // the safe separator and sanitized; original kept in wire_to_remote_.
    std::string cache_wire_name(const McpToolInfo& info);

    McpServerConfig config_;
    TransportFactory factory_;
    std::chrono::milliseconds handshake_timeout_;
    std::chrono::milliseconds call_timeout_;
    std::unique_ptr<McpRpcClient> rpc_;
    bool down_ = false;
    bool initialized_ = false;
    bool tools_capability_ = false;
    std::string negotiated_version_;
    std::string last_error_;
    std::vector<McpToolInfo> tool_cache_;
    bool cache_valid_ = false;
    // Wire name (registry-visible, [A-Za-z0-9_-]) -> original server name.
    std::map<std::string, std::string> wire_to_remote_;
};

// Feed the negotiated protocol version into an HTTP transport so
// follow-up requests carry MCP-Protocol-Version (2025-06-18 MUST).
// No-op for non-HTTP transports.
void set_http_transport_version(McpTransport& transport, const std::string& version);

// Factory bundle: which transport per config. mcp_stdio.cpp / mcp_http.cpp
// register these; main() wires both, tests inject fakes.
std::unique_ptr<McpTransport> make_stdio_transport(const McpServerConfig& config);
std::unique_ptr<McpTransport> make_http_transport(const McpServerConfig& config);
// Routes by config.transport ("stdio" default).
TransportFactory default_transport_factory();

} // namespace mini_agent
