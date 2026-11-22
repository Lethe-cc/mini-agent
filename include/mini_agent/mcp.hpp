#pragma once

#include <atomic>
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

// Session seam between the MCP dialogue (McpClient below) and a JSON-RPC
// backend (design 021 §5.1). Two implementations: the legacy McpRpcClient
// (id correlation over one McpTransport) and the cxxmcp session backed by
// the vendored SDK (src/mcp/mcp_cxxmcp.cpp). The contract mirrors the
// legacy error-text surface so facade behavior is backend-independent.
// The header stays SDK-free and nlohmann-free.
class McpSession {
  public:
    virtual ~McpSession() = default;
    // Open the channel (spawn subprocess / prepare endpoint). Idempotent;
    // false + *error on failure.
    virtual bool start(std::string* error) = 0;
    virtual bool alive() const noexcept = 0;
    virtual void close() noexcept = 0;
    // Sends a request and waits for the matching response. On success
    // *result_json carries the raw "result" node ("{}" when absent);
    // JSON-RPC "error" replies set *error (with code) and return false.
    virtual bool request(std::string_view method, const std::string& params_json,
                         std::chrono::milliseconds timeout, std::string* result_json,
                         std::string* error) = 0;
    // Fire-and-forget notification; transport failure is reported, a
    // server 202/absent reply is success by definition.
    virtual bool notify(std::string_view method, const std::string& params_json,
                        std::string* error) = 0;
    // Feed the negotiated protocol version to backends that stamp it on
    // the wire (HTTP MCP-Protocol-Version). No-op by default.
    virtual void set_protocol_version(const std::string& version) {
        (void)version;
    }
    // M2a (design 022 §5.1): notifications/*/list_changed observer. The
    // callback runs on a TRANSPORT thread and must only touch atomics;
    // kind is "tools", "resources" or "prompts". No-op by default.
    virtual void set_list_changed_callback(std::function<void(std::string_view)> cb) {
        (void)cb;
    }
};

// JSON-RPC 2.0 client over one transport: monotonic ids, strictly serial
// in-flight requests (v2.0 design), foreign-id messages ignored except
// the three list_changed notifications, which reach the observer installed
// via set_list_changed_callback (design 022 §5.1); server requests are
// outside our capability set and folded away (design 008 section 5.2).
// Not a general pool -- one connection per server.
class McpRpcClient final : public McpSession {
  public:
    explicit McpRpcClient(std::unique_ptr<McpTransport> transport)
        : transport_(std::move(transport)) {
    }

    bool start(std::string* error) override;
    bool alive() const noexcept override;
    void close() noexcept override;
    McpTransport& transport() noexcept {
        return *transport_;
    }

    bool request(std::string_view method, const std::string& params_json,
                 std::chrono::milliseconds timeout, std::string* result_json,
                 std::string* error) override;
    bool notify(std::string_view method, const std::string& params_json,
                std::string* error) override;
    void set_protocol_version(const std::string& version) override;
    void set_list_changed_callback(std::function<void(std::string_view)> cb) override;

  private:
    std::unique_ptr<McpTransport> transport_;
    long long next_id_ = 1;
    std::function<void(std::string_view)> list_changed_cb_;
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
// `call()` performs ONE transparent restart+re-handshake+retry when the
// transport died (stdio child crashed / was killed, HTTP session broken):
// without it a crashed server would stay down for the whole process
// because nothing ever called restart(). A retry that fails again marks
// the client down and surfaces the error as before.
//
// Threading: NOT internally synchronized -- one client instance is owned
// by exactly one caller at a time (CLI: the turn; TUI: the worker between
// joins, main thread rebuilding only after join). list()/call()/restart()
// mutate the cache and transport; concurrent use is a contract violation.
// Sole exception: list_changed notifications set atomic invalidation flags
// from a transport thread (M2a, design 022 §5.1) -- never anything else.
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
    std::string source_name() const override {
        return config_.name;
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
    // M2a capability probes (design 022 §5.1): true only after a handshake
    // that advertised the capability. The synthetic tools/list surface
    // and the system-prompt hint gate on these.
    bool has_resources() const noexcept {
        return resources_capability_;
    }
    bool has_prompts() const noexcept {
        return prompts_capability_;
    }
    bool initialized() const noexcept {
        return initialized_;
    }

    // M2a resource/prompt seam (design 022 §5.1). JSON strings in/out --
    // the header stays nlohmann-free. All lazy (ensure_started on first
    // use), all failures folded into *error, never thrown. list_* return
    // the aggregated metadata array (bounded 16-page walk, same cap as
    // tools/list); cached until a list_changed notification or restart.
    // read_resource returns the raw "contents" array; get_prompt returns
    // the raw prompts/get result. args_json must be a JSON object ("{}"
    // or empty = no arguments).
    std::string list_resources(std::string* error);
    std::string read_resource(const std::string& uri, std::string* error);
    std::string list_prompts(std::string* error);
    std::string get_prompt(const std::string& name, const std::string& args_json,
                           std::string* error);

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
    // One cached list walk (16 pages max) over resources/list or
    // prompts/list; item_key is the array member ("resources"/"prompts").
    std::string list_paged(std::string_view method, std::string_view item_key,
                           std::string* cache, std::atomic<bool>& stale,
                           std::string* error);
    // Synthetic <server>/mcp_resources|mcp_prompts dispatch (action
    // list|read / list|get); never sent to the remote tools/call path.
    ToolResult call_synthetic(const ToolCall& call, bool resources);

    McpServerConfig config_;
    TransportFactory factory_;
    std::chrono::milliseconds handshake_timeout_;
    std::chrono::milliseconds call_timeout_;
    std::unique_ptr<McpSession> rpc_;
    bool down_ = false;
    bool initialized_ = false;
    bool tools_capability_ = false;
    std::string negotiated_version_;
    std::string last_error_;
    std::vector<McpToolInfo> tool_cache_;
    bool cache_valid_ = false;
    // Wire name (registry-visible, [A-Za-z0-9_-]) -> original server name.
    std::map<std::string, std::string> wire_to_remote_;
    // M2a state. Capability flags follow tools_capability_ (owner thread
    // only). List caches hold aggregated JSON arrays; the stale flags are
    // written by the transport-thread notification observer and read by
    // the owner, hence atomic. Content is NEVER cached (design 022 T2).
    bool resources_capability_ = false;
    bool prompts_capability_ = false;
    std::string resources_cache_;
    std::string prompts_cache_;
    std::atomic<bool> tools_stale_{false};
    std::atomic<bool> resources_stale_{false};
    std::atomic<bool> prompts_stale_{false};
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

// cxxmcp backend session (design 021 §5.1, ADR 010): defined in
// src/mcp/mcp_cxxmcp.cpp, the only TU that includes the vendored SDK.
// The DEFAULT when the McpClient factory is EMPTY (production routing,
// used by build_tool_registry); MINI_AGENT_MCP_BACKEND=legacy opts back
// to the self-written path (rollback hatch until M3 removes it). Any
// non-null factory -- including default_transport_factory() -- forces
// the legacy path; test fakes rely on this.
std::unique_ptr<McpSession> make_cxxmcp_session(const McpServerConfig& config,
                                                std::chrono::milliseconds handshake_timeout,
                                                std::chrono::milliseconds call_timeout);

} // namespace mini_agent
