#include "mini_agent/mcp.hpp"

#include <cxxmcp/client/client.hpp>
#include <cxxmcp/client/http_transport.hpp>
#include <cxxmcp/protocol/prompt.hpp>
#include <cxxmcp/protocol/resource.hpp>
#include <cxxmcp/protocol/serialization.hpp>
#include <cxxmcp/request.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mini_agent {
namespace {

namespace client = mcp::client;
namespace protocol = mcp::protocol;
using mcp::core::Error;
using mcp::core::Unit;

constexpr std::size_t kMaxLineBytes = 1024u * 1024u; // parity with mcp_stdio.cpp
constexpr std::size_t kStderrTailBytes = 4096u;

// ---------------------------------------------------------------------------
// stdio bridge: an SDK request-level Transport over the same child-process
// hardening as mcp_stdio.cpp (clearenv + explicit env, setpgid, PDEATHSIG,
// 1 MiB framing cap, stderr diagnostics tail, EOF-drain + group SIGKILL).
// The spawn/framing logic is duplicated deliberately so the legacy default
// backend stays untouched during the M1 migration window (ADR 010).
// ---------------------------------------------------------------------------
class StdioBridgeTransport final : public client::Transport {
  public:
    explicit StdioBridgeTransport(McpServerConfig config) : config_(std::move(config)) {
    }
    ~StdioBridgeTransport() override {
        stop();
    }

    // The session sets the per-request deadline before each SDK call:
    // handshake_timeout_ for initialize, call_timeout_ for the rest.
    void set_deadline(std::chrono::milliseconds timeout) {
        deadline_ms_ = timeout;
    }

    bool spawn(std::string* error) {
        if (alive())
            return true;
        int in_pipe[2];  // client -> child stdin
        int out_pipe[2]; // child stdout -> client
        int err_pipe[2]; // child stderr -> client (diagnostics only)
        if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
            if (error)
                *error = std::string("pipe failed: ") + std::strerror(errno);
            return false;
        }
        const ::pid_t pid = ::fork();
        if (pid < 0) {
            if (error)
                *error = std::string("fork failed: ") + std::strerror(errno);
            return false;
        }
        if (pid == 0) {
            // Child: same security posture as the legacy stdio transport.
            ::dup2(in_pipe[0], STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(in_pipe[0]);
            ::close(in_pipe[1]);
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
            ::close(err_pipe[0]);
            ::close(err_pipe[1]);
            ::setpgid(0, 0);
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() == 1)
                ::_exit(126);
            ::clearenv();
            for (const auto& kv : config_.env)
                ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
            if (::getenv("PATH") == nullptr)
                ::setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(config_.command.c_str()));
            for (const auto& arg : config_.args)
                argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            ::execvp(config_.command.c_str(), argv.data());
            ::_exit(127);
        }
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
        child_ = pid;
        to_child_ = in_pipe[1];
        from_child_ = out_pipe[0];
        from_stderr_ = err_pipe[0];
        exited_ = false;
        return true;
    }

    bool alive() const noexcept {
        return child_ > 0 && !exited_;
    }

    std::string diagnostics_tail() const {
        return stderr_tail_;
    }

    mcp::core::Result<protocol::JsonRpcResponse> send(
        const protocol::JsonRpcRequest& request) override {
        std::string error;
        if (!alive())
            return failure<protocol::JsonRpcResponse>(protocol::ErrorCode::InternalError,
                                                      "mcp transport is closed");
        auto serialized = protocol::serialize_request(request);
        if (!serialized)
            return mcp::core::unexpected(serialized.error());
        if (!write_line(*serialized, &error))
            return failure<protocol::JsonRpcResponse>(protocol::ErrorCode::InternalError, error);
        return await_response(request.id, request.method);
    }

    // The SDK default rejects notifications on custom transports; stdio
    // framing supports them natively (notifications/initialized MUST go out).
    mcp::core::Result<Unit> send_notification(
        const protocol::JsonRpcNotification& notification) override {
        if (!alive())
            return failure<Unit>(protocol::ErrorCode::InternalError, "mcp transport is closed");
        auto serialized = protocol::serialize_notification(notification);
        if (!serialized)
            return mcp::core::unexpected(serialized.error());
        std::string error;
        if (!write_line(*serialized, &error))
            return failure<Unit>(protocol::ErrorCode::InternalError, error);
        return Unit{};
    }

    mcp::core::Result<Unit> start(client::TransportRequestHandler request_handler,
                                  client::TransportNotificationHandler notification_handler)
        override {
        (void)request_handler; // inbound server requests: capability none
        // M2a: keep the SDK notification dispatcher so folded id-less lines
        // reach the list_changed observers (design 022 §5.1). The bridge is
        // synchronous: it fires on the owner thread inside await_response.
        notification_handler_ = std::move(notification_handler);
        std::string error;
        if (!spawn(&error))
            return failure<Unit>(protocol::ErrorCode::InternalError, error);
        return Unit{};
    }

    void stop() noexcept override {
        if (child_ <= 0)
            return;
        if (!exited_) {
            if (to_child_ >= 0)
                ::close(to_child_); // EOF: well-behaved servers exit
            to_child_ = -1;
            for (int i = 0; i < 50 && !reaped_if_dead(); ++i) {
                pump_stderr();
                pump_output();
                struct timespec nap{0, 20000000}; // 20 ms
                ::nanosleep(&nap, nullptr);
            }
            if (!exited_) {
                ::kill(-child_, SIGKILL); // group: cover grandchild trees
                ::waitpid(child_, nullptr, 0);
                exited_ = true;
            }
        }
        if (to_child_ >= 0)
            ::close(to_child_);
        if (from_child_ >= 0)
            ::close(from_child_);
        if (from_stderr_ >= 0)
            ::close(from_stderr_);
        to_child_ = from_child_ = from_stderr_ = -1;
        child_ = -1;
    }

  private:
    template <typename T>
    static mcp::core::Result<T> failure(protocol::ErrorCode code, std::string message) {
        return mcp::core::unexpected(
            Error{static_cast<int>(code), std::move(message), {}, "transport"});
    }

    bool write_line(const std::string& payload, std::string* error) {
        if (!alive() || to_child_ < 0) {
            *error = "mcp child process is not running";
            return false;
        }
        std::string line = payload;
        line += '\n';
        std::size_t written = 0;
        while (written < line.size()) {
            const ssize_t n = ::write(to_child_, line.data() + written, line.size() - written);
            if (n > 0) {
                written += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EAGAIN) {
                struct pollfd pfd{to_child_, POLLOUT, 0};
                ::poll(&pfd, 1, 100);
                if (reaped_if_dead())
                    break;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break; // EPIPE etc: child gone
        }
        if (written < line.size()) {
            *error = "write to mcp child failed (exit? )";
            return false;
        }
        return true;
    }

    // id-matched receive in 200 ms slices until the session-set deadline;
    // foreign ids (server notifications/requests) are folded away exactly
    // like the legacy McpRpcClient loop.
    mcp::core::Result<protocol::JsonRpcResponse> await_response(const protocol::RequestId& id,
                                                                const std::string& method) {
        const auto deadline = std::chrono::steady_clock::now() + deadline_ms_;
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero())
                return failure<protocol::JsonRpcResponse>(
                    protocol::ErrorCode::InternalError, "mcp request timed out (" + method + ")");
            std::string recv_error;
            auto next = recv_line(std::min(remaining, std::chrono::milliseconds(200)),
                                  &recv_error);
            if (recv_error.empty() && !next.has_value())
                continue; // poll slice elapsed, deadline still open
            if (!next.has_value())
                return failure<protocol::JsonRpcResponse>(protocol::ErrorCode::InternalError,
                                                          recv_error);
            auto reply = nlohmann::json::parse(*next, nullptr, false);
            if (reply.is_discarded() || !reply.is_object())
                return failure<protocol::JsonRpcResponse>(protocol::ErrorCode::InternalError,
                                                          "mcp server sent a non-JSON-RPC line");
            auto id_it = reply.find("id");
            if (id_it == reply.end() || !id_it->is_number_integer() ||
                !id_matches(*id_it, id)) {
                if (id_it == reply.end() && notification_handler_) {
                    // Id-less line = notification: hand it to the SDK
                    // dispatcher (list_changed etc); unknown methods are
                    // ignored there. Server requests WITH an id stay folded.
                    if (auto notification = protocol::parse_notification(*next))
                        (void)notification_handler_(*notification);
                }
                continue;
            }
            auto parsed = protocol::parse_response(*next);
            if (!parsed)
                return mcp::core::unexpected(parsed.error());
            return *parsed;
        }
    }

    static bool id_matches(const nlohmann::json& reply_id, const protocol::RequestId& id) {
        if (const auto* integer = std::get_if<std::int64_t>(&id))
            return reply_id.get<long long>() == static_cast<long long>(*integer);
        return false; // we only ever send integer ids
    }

    std::optional<std::string> recv_line(std::chrono::milliseconds timeout, std::string* error) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (auto line = take_line())
                return line;
            if (error_out_) {
                *error = "mcp stdout exceeds 1 MiB per message";
                error_out_ = false;
                return std::nullopt;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero())
                return std::nullopt; // pure timeout, no error
            struct pollfd fds[2];
            fds[0] = pollfd{from_child_, POLLIN, 0};
            const bool have_stderr = from_stderr_ >= 0;
            if (have_stderr)
                fds[1] = pollfd{from_stderr_, POLLIN, 0};
            const nfds_t count = have_stderr ? 2 : 1;
            const int pr = ::poll(fds, count, static_cast<int>(remaining.count()));
            if (pr > 0) {
                if (have_stderr && (fds[1].revents & POLLIN))
                    pump_stderr();
                if (fds[0].revents & POLLIN) {
                    pump_output();
                } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    if (!pump_output()) { // EOF: any tail bytes still readable?
                        if (take_line())
                            continue;
                        *error = "mcp child stdout closed";
                        exited_ = true;
                        return std::nullopt;
                    }
                }
            }
            reaped_if_dead();
        }
    }

    bool pump_output() {
        if (from_child_ < 0)
            return true;
        char buffer[8192];
        for (;;) {
            const ssize_t n = ::read(from_child_, buffer, sizeof(buffer));
            if (n > 0) {
                buffer_.append(buffer, static_cast<std::size_t>(n));
                if (buffer_.size() > kMaxLineBytes)
                    error_out_ = true;
                continue; // drain until EAGAIN
            }
            if (n == 0)
                return false; // EOF
            return true;      // EAGAIN/EINTR
        }
    }

    std::optional<std::string> take_line() {
        const auto nl = buffer_.find('\n');
        if (nl == std::string::npos)
            return std::nullopt;
        std::string line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            return take_line(); // skip blank lines defensively
        return line;
    }

    bool reaped_if_dead() {
        if (exited_ || child_ <= 0)
            return exited_;
        const pid_t r = ::waitpid(child_, nullptr, WNOHANG);
        if (r == child_) {
            exited_ = true;
            pump_output();
        }
        return exited_;
    }

    void pump_stderr() {
        if (from_stderr_ < 0)
            return;
        char buffer[2048];
        const ssize_t n = ::read(from_stderr_, buffer, sizeof(buffer));
        if (n > 0) {
            stderr_tail_.append(buffer, static_cast<std::size_t>(n));
            if (stderr_tail_.size() > kStderrTailBytes * 2)
                stderr_tail_.erase(0, stderr_tail_.size() - kStderrTailBytes);
        }
    }

    McpServerConfig config_;
    client::TransportNotificationHandler notification_handler_;
    std::chrono::milliseconds deadline_ms_{10000};
    pid_t child_ = -1;
    int to_child_ = -1;
    int from_child_ = -1;
    int from_stderr_ = -1;
    bool exited_ = false;
    bool error_out_ = false;
    std::string buffer_;
    std::string stderr_tail_;
};

// ---------------------------------------------------------------------------
// CxxmcpSession: McpSession over the vendored low-level Client. Sync-only
// by construction (never request_async): the SDK destructor wait on the
// in-flight counter is then trivially satisfied and the M0 hang class is
// structurally excluded. Error texts are mapped back to the legacy surface
// wherever a stable string exists; genuine deviations are logged in
// design 021 §9.
// ---------------------------------------------------------------------------
class CxxmcpSession final : public McpSession {
  public:
    CxxmcpSession(McpServerConfig config, std::chrono::milliseconds handshake_timeout,
                  std::chrono::milliseconds call_timeout)
        : config_(std::move(config)), handshake_timeout_(handshake_timeout) {
        // call_timeout arrives per request() instead (stdio deadline); HTTP
        // keeps the legacy fixed 120 s curl budget, matching mcp_http.cpp
        // which also ignores per-call timeouts.
        (void)call_timeout;
    }

    ~CxxmcpSession() override {
        close();
    }

    bool start(std::string* error) override {
        if (dead_) {
            if (error)
                *error = "mcp transport is closed";
            return false;
        }
        if (started_)
            return true;
        if (config_.transport == "http") {
            if (config_.url.empty()) {
                if (error)
                    *error = "http transport requires url";
                return false;
            }
            client::HttpTransportOptions options;
            options.uri = config_.url;
            if (!config_.bearer_token.empty())
                options.auth_header = config_.bearer_token;
            // Legacy curl budget: 120 s overall (CONNECTTIMEOUT was 10 s;
            // the SDK uses one value for connect/read/write -- §9).
            options.timeout = std::chrono::seconds(120);
            http_ = new client::HttpTransport(std::move(options));
            client_ = std::make_unique<client::Client>(std::unique_ptr<client::Transport>(http_));
        } else {
            auto bridge = std::make_unique<StdioBridgeTransport>(config_);
            std::string spawn_error;
            if (!bridge->spawn(&spawn_error)) {
                if (error)
                    *error = spawn_error;
                return false;
            }
            stdio_ = bridge.get();
            client_ = std::make_unique<client::Client>(std::move(bridge));
        }
        // Read-only capabilities client, exactly like the legacy handshake:
        // without this the SDK advertises roots/sampling/elicitation by
        // default (M2b adds the server-request bridge, design 022 §5.2).
        client_->set_capabilities(protocol::ClientCapabilities{});
        // M2a observers: HTTP notifications arrive on the SSE receive
        // thread, stdio on the owner thread. The callback only stores an
        // atomic stale flag in the facade (design 022 §5.1).
        const auto forward = [this](const char* kind) {
            std::lock_guard<std::mutex> lock(list_changed_mutex_);
            if (list_changed_cb_)
                list_changed_cb_(kind);
        };
        client_->on_tool_list_changed([forward] { forward("tools"); });
        client_->on_resource_list_changed([forward] { forward("resources"); });
        client_->on_prompt_list_changed([forward] { forward("prompts"); });
        started_ = true;
        return true;
    }

    bool alive() const noexcept override {
        if (!started_ || dead_)
            return false;
        if (stdio_)
            return stdio_->alive();
        return true; // HTTP is stateless-per-POST; only auth kills it
    }

    void close() noexcept override {
        started_ = false;
        dead_ = true;
        if (client_)
            client_->stop(); // stdio: EOF-drain + kill; HTTP: DELETE + SSE join
        // Detach AFTER stop(): the HTTP receive thread is joined by then,
        // so no observer can be mid-call. The mutex closes the remaining
        // window for stateless/edge paths (design 022 §5.1 shutdown order).
        {
            std::lock_guard<std::mutex> lock(list_changed_mutex_);
            list_changed_cb_ = {};
        }
        client_.reset();
        stdio_ = nullptr;
        http_ = nullptr;
    }

    bool request(std::string_view method, const std::string& params_json,
                 std::chrono::milliseconds timeout, std::string* result_json,
                 std::string* error) override {
        if (!started_ || !alive()) {
            if (error)
                *error = "mcp transport is closed";
            return false;
        }
        nlohmann::json params = nlohmann::json::object();
        if (!params_json.empty() && params_json != "{}") {
            params = nlohmann::json::parse(params_json, nullptr, false);
            if (params.is_discarded()) {
                if (error)
                    *error = "internal: params_json is not valid JSON";
                return false;
            }
        }
        if (stdio_)
            stdio_->set_deadline(timeout);

        if (method == "initialize") {
            // Client::initialize owns the lifecycle request; the facade
            // keeps its own version/capability validation on the payload.
            mcp::RequestOptions options;
            if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
                options.protocol_version = params["protocolVersion"].get<std::string>();
            options.timeout = timeout;
            auto payload = client_->initialize("mini-agent", "0.2", std::move(options));
            if (!payload) {
                map_error(payload.error(), method, error);
                return false;
            }
            if (result_json)
                *result_json = payload->dump();
            return true;
        }

        // M2a typed surface (design 022 §5.1): one page per request; the
        // facade owns the 16-page cap on both backends. Typed calls stamp
        // the negotiated HTTP version themselves (auto-remembered).
        if (method == "resources/list" || method == "prompts/list") {
            auto page_params = protocol::paginated_request_params_from_json(params);
            if (!page_params) {
                if (error)
                    *error = "rpc error " +
                             std::to_string(static_cast<int>(protocol::ErrorCode::InvalidParams)) +
                             ": pagination params must be an object with a string cursor";
                return false;
            }
            if (method == "resources/list") {
                auto payload = client_->list_resources_page(*page_params);
                if (!payload) {
                    map_error(payload.error(), method, error);
                    return false;
                }
                if (result_json)
                    *result_json = protocol::resources_list_result_to_json(*payload).dump();
            } else {
                auto payload = client_->list_prompts_page(*page_params);
                if (!payload) {
                    map_error(payload.error(), method, error);
                    return false;
                }
                if (result_json)
                    *result_json = protocol::prompts_list_result_to_json(*payload).dump();
            }
            return true;
        }
        if (method == "resources/read") {
            auto uri = params.find("uri");
            if (uri == params.end() || !uri->is_string()) {
                if (error)
                    *error = "rpc error " +
                             std::to_string(static_cast<int>(protocol::ErrorCode::InvalidParams)) +
                             ": resources/read requires a uri string";
                return false;
            }
            auto payload = client_->read_resource(uri->get_ref<const std::string&>());
            if (!payload) {
                map_error(payload.error(), method, error);
                return false;
            }
            if (result_json)
                *result_json = protocol::resources_read_result_to_json(*payload).dump();
            return true;
        }
        if (method == "prompts/get") {
            auto name = params.find("name");
            if (name == params.end() || !name->is_string()) {
                if (error)
                    *error = "rpc error " +
                             std::to_string(static_cast<int>(protocol::ErrorCode::InvalidParams)) +
                             ": prompts/get requires a name string";
                return false;
            }
            protocol::Json arguments = protocol::Json::object();
            if (auto a = params.find("arguments"); a != params.end() && a->is_object())
                arguments = *a;
            auto payload = client_->get_prompt(name->get_ref<const std::string&>(), arguments);
            if (!payload) {
                map_error(payload.error(), method, error);
                return false;
            }
            if (result_json)
                *result_json = protocol::prompts_get_result_to_json(*payload).dump();
            return true;
        }

        auto request = protocol::make_request(std::string(method), next_id_++, std::move(params));
        if (http_ && !negotiated_version_.empty())
            request.protocol_version_override = negotiated_version_;
        auto payload = client_->raw_request(request);
        if (!payload) {
            const auto& err = payload.error();
            if (err.message == "response did not contain a result") {
                // Legacy parity: a reply without result/error is "{}".
                if (result_json)
                    *result_json = "{}";
                return true;
            }
            map_error(err, method, error);
            return false;
        }
        if (result_json)
            *result_json = payload->dump();
        return true;
    }

    bool notify(std::string_view method, const std::string& params_json,
                std::string* error) override {
        if (!started_ || !alive()) {
            if (error)
                *error = "mcp transport is closed";
            return false;
        }
        nlohmann::json params = nlohmann::json::object();
        if (!params_json.empty() && params_json != "{}") {
            params = nlohmann::json::parse(params_json, nullptr, false);
            if (params.is_discarded())
                params = nlohmann::json::object(); // legacy drops bad params silently
        }
        if (stdio_)
            stdio_->set_deadline(handshake_timeout_);
        auto notification = protocol::make_notification(std::string(method), std::move(params));
        auto sent = client_->raw_notification(notification);
        if (!sent) {
            map_error(sent.error(), method, error);
            return false;
        }
        return true;
    }

    void set_protocol_version(const std::string& version) override {
        negotiated_version_ = version;
        if (http_)
            http_->set_negotiated_protocol_version(version);
    }

    void set_list_changed_callback(std::function<void(std::string_view)> cb) override {
        std::lock_guard<std::mutex> lock(list_changed_mutex_);
        list_changed_cb_ = std::move(cb);
    }

  private:
    // Map SDK core::Error back to the legacy error-text surface. Death
    // semantics stay transport-derived in the facade: stdio alive() flips
    // when the child dies; HTTP flips only on auth refusal (legacy rule).
    void map_error(const Error& err, std::string_view method, std::string* error) {
        if (!error)
            return;
        const std::string& message = err.message;
        if (err.category == "auth") {
            dead_ = true;
            *error = "mcp server refused authentication (HTTP " + auth_status(err) +
                     "); bearer token missing or invalid (OAuth not supported)";
            return;
        }
        if (message == "http transport request failed") {
            *error = "mcp http network error: " + err.detail;
            return;
        }
        if (message == "http transport request timed out") {
            *error = "mcp http network error: timed out (" + err.detail + ")";
            return; // "timed out" substring drives the facade desync rule
        }
        if (message == "http transport request returned an error status") {
            *error = "mcp http status " + err.detail;
            return;
        }
        if (message == "http transport notification returned an error status") {
            *error = "mcp http status " + err.detail;
            return;
        }
        if (message == "http transport session was terminated") {
            // The SDK usually auto-recovers a 404 internally; this text
            // only surfaces when that recovery itself failed (§9).
            *error = "mcp session expired; re-initialize required";
            return;
        }
        if (message == "http transport notification failed") {
            *error = "mcp http network error: " + err.detail;
            return;
        }
        if (message.rfind("unsupported MCP protocol version(\"", 0) == 0) {
            // Legacy wording for a server version we do not implement.
            std::string version = message.substr(32);
            if (version.size() >= 2 && version.compare(version.size() - 2, 2, "\")") == 0)
                version.erase(version.size() - 2);
            *error = "server wants protocol version " + version +
                     " which this client does not implement";
            return;
        }
        if (err.category == "protocol" && err.detail.empty() &&
            message.rfind("rpc error", 0) != 0 && is_rpc_error_code(err.code)) {
            // JSON-RPC error replies arrive as protocol errors without
            // detail; re-attach the legacy "rpc error <code>: " prefix.
            // SDK-internal failures reuse some codes (e.g. InvalidRequest
            // for strict-parse errors); those texts stay SDK-specific
            // (design 021 §9).
            *error = "rpc error " + std::to_string(err.code) + ": " + message;
            return;
        }
        *error = message.empty() ? std::string("mcp request failed (" + std::string(method) + ")")
                                 : message;
    }

    static bool is_rpc_error_code(int code) {
        return code == -32700 || code == -32600 || code == -32601 || code == -32602 ||
               code == -32603 || (code >= -32099 && code <= -32000);
    }

    static std::string auth_status(const Error& err) {
        // detail carries the numeric status, or the WWW-Authenticate value
        // when the server sent one; recover the status from the message.
        if (!err.detail.empty() &&
            err.detail.find_first_not_of("0123456789") == std::string::npos)
            return err.detail;
        return err.message == "http transport request has insufficient scope" ? "403" : "401";
    }

    McpServerConfig config_;
    std::chrono::milliseconds handshake_timeout_;
    std::unique_ptr<client::Client> client_;
    StdioBridgeTransport* stdio_ = nullptr; // owned by client_
    client::HttpTransport* http_ = nullptr; // owned by client_
    std::string negotiated_version_;
    std::int64_t next_id_ = 1;
    bool started_ = false;
    bool dead_ = false;
    // Guards cb lifetime against the HTTP SSE receive thread; the callback
    // itself only stores atomics (design 022 §5.1).
    std::mutex list_changed_mutex_;
    std::function<void(std::string_view)> list_changed_cb_;
};

} // namespace

std::unique_ptr<McpSession> make_cxxmcp_session(const McpServerConfig& config,
                                                std::chrono::milliseconds handshake_timeout,
                                                std::chrono::milliseconds call_timeout) {
    return std::make_unique<CxxmcpSession>(config, handshake_timeout, call_timeout);
}

} // namespace mini_agent