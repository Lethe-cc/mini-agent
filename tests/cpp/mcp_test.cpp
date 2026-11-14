// Unit + transport tests for the v2 MCP client (design 008 section 8):
// RPC core over a scripted fake transport, McpClient dialogue rules, and
// a real-child-process stdio round trip against tests/mock_mcp_server.py
// (skipped when python3 or the script is absent). Plain asserts, no
// framework dependency, mirroring agent_test.cpp.
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mini_agent/config.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/tools.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// Records sent messages; replays a canned reply batch after each send.
class FakeTransport final : public mini_agent::McpTransport {
  public:
    bool start(std::string*) override {
        alive_ = true;
        return true;
    }
    bool alive() const override {
        return alive_;
    }
    bool send(std::string_view message, std::string*) override {
        sent_.push_back(std::string(message));
        const std::size_t index = sent_.size() - 1;
        if (index < reply_script_.size())
            for (const auto& r : reply_script_[index])
                inbox_.push_back(r);
        // v4.17 auto-restart tests: simulate a channel that dies mid-
        // request (child crash after the message went out, no reply).
        if (die_after_sends_ > 0 && sent_.size() == static_cast<std::size_t>(die_after_sends_))
            alive_ = false;
        return true;
    }
    std::optional<std::string> recv(std::chrono::milliseconds, std::string* error) override {
        if (inbox_.empty()) {
            // No scripted reply left: report a dead channel so a mistake
            // in the test matrix fails fast instead of spinning to the
            // request timeout. Tests that WANT a silent channel clear the
            // flag.
            if (error)
                *error =
                    fail_on_empty_ ? "no scripted reply (test matrix error)" : "channel closed";
            return std::nullopt;
        }
        std::string front = inbox_.front();
        inbox_.erase(inbox_.begin());
        return front;
    }
    void close() noexcept override {
        alive_ = false;
    }

    std::vector<std::string> sent_;
    std::vector<std::vector<std::string>> reply_script_;
    std::vector<std::string> inbox_;
    bool fail_on_empty_ = false;
    bool alive_ = false;
    int die_after_sends_ = 0; // >0: channel dies after that many sends
};

std::string reply_to(long long id, const std::string& result_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + result_json + "}";
}

mini_agent::McpServerConfig server_config(const std::string& name) {
    mini_agent::McpServerConfig config;
    config.name = name;
    config.transport = "stdio";
    config.command = "unused";
    return config;
}

// The factory moves the transport out on first use; tests keep the fake's
// raw pointer for assertions on recorded traffic.
FakeTransport* install_fake(std::unique_ptr<mini_agent::McpTransport>& slot) {
    auto fake = std::make_unique<FakeTransport>();
    auto* raw = fake.get();
    slot = std::move(fake);
    return raw;
}

void test_handshake_and_tool_round_trip() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {reply_to(1, R"({"protocolVersion":"2025-03-26","capabilities":{"tools":{}}})")},
        {},
        {reply_to(
            2,
            R"({"tools":[{"name":"add","description":"add nums","inputSchema":{"type":"object"}}]})")},
        {reply_to(3, R"({"content":[{"type":"text","text":"7"}],"isError":false})")},
        {reply_to(4, R"({"content":[{"type":"text","text":"8"}],"isError":false})")},
    };
    McpClient client(server_config("fake"),
                     [&slot](const McpServerConfig&) { return std::move(slot); });

    auto defs = client.list();
    check(defs.size() == 1 && defs[0].name == "fake__add" && defs[0].source == "fake",
          "tools listed with server namespace prefix");
    check(client.protocol_version() == "2025-03-26", "negotiated version recorded");
    check(fake->sent_.size() == 3, "initialize + initialized + tools/list sent");
    check(fake->sent_[0].find(R"("protocolVersion":"2025-06-18")") != std::string::npos,
          "client offers latest version first");
    check(fake->sent_[1].find("notifications/initialized") != std::string::npos,
          "initialized notification mandatory");

    const auto result =
        client.call(ToolCall{"t1", "fake__add", R"({"a":1})"}, [] { return false; });
    check(!result.is_error && result.text == "7", "tools/call round trip");
    check(fake->sent_.back().find(R"("name":"add")") != std::string::npos,
          "namespaced name mapped back to server name");
    check(fake->sent_.back().find(R"("arguments":{"a":1})") != std::string::npos,
          "arguments forwarded verbatim");

    // second call: the tools/list cache keeps traffic minimal
    const auto before = fake->sent_.size();
    client.call(ToolCall{"t2", "fake__add", "{}"}, [] { return false; });
    check(fake->sent_.size() == before + 1, "tools/list cached per process");

    // hot path (review 2026-11-05): re-listing with a valid cache must be
    // fully network-free -- run_turn asks the registry every provider
    // round, so any traffic here would add one RTT per tool round
    const auto relisted = client.list();
    check(relisted.size() == 1 && fake->sent_.size() == before + 1,
          "list() with valid cache sends nothing");
}

void test_foreign_and_error_messages() {
    using namespace mini_agent;

    // server-initiated message before our reply: skipped, not an error
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {R"X({"jsonrpc":"2.0","method":"notifications/message"})X",
         reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
        {},
        {reply_to(2, R"({"tools":[]})")},
    };
    McpClient client1(server_config("x"),
                      [&slot](const McpServerConfig&) { return std::move(slot); });
    check(client1.list().empty(), "foreign message skipped; empty list ok");

    // JSON-RPC error on initialize marks the client down
    std::unique_ptr<McpTransport> slot2;
    auto* fake2 = install_fake(slot2);
    fake2->reply_script_ = {
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32600,"
         "\"message\":\"not an MCP server\"}}"},
    };
    McpClient client2(server_config("bad"),
                      [&slot2](const McpServerConfig&) { return std::move(slot2); });
    check(client2.list().empty(), "handshake failure yields no tools");
    check(!client2.alive(), "client marked down after handshake error");
    const auto result = client2.call(ToolCall{"t", "bad__x", "{}"}, [] { return false; });
    check(result.is_error && result.text.find("down") != std::string::npos,
          "call on down server is an error result, not a crash");
    check(client2.last_error().find("not an MCP server") != std::string::npos,
          "last_error surfaces handshake reason");

    // version we do not implement: disconnect per spec
    std::unique_ptr<McpTransport> slot3;
    auto* fake3 = install_fake(slot3);
    fake3->reply_script_ = {
        {reply_to(1, R"({"protocolVersion":"2099-01-01","capabilities":{}})")},
    };
    McpClient client3(server_config("future"),
                      [&slot3](const McpServerConfig&) { return std::move(slot3); });
    check(client3.list().empty() && !client3.alive(),
          "unsupported negotiated version marks server down");

    // isError result from tools/call maps to ToolResult.is_error
    std::unique_ptr<McpTransport> slot4;
    auto* fake4 = install_fake(slot4);
    fake4->reply_script_ = {
        {reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
        {},
        {reply_to(2, R"({"content":[{"type":"text","text":"bad input"}],"isError":true})")},
    };
    McpClient client4(server_config("err"),
                      [&slot4](const McpServerConfig&) { return std::move(slot4); });
    const auto failed = client4.call(ToolCall{"t", "err__boom", "{}"}, [] { return false; });
    check(failed.is_error && failed.text == "bad input", "isError flag preserved");
    check(client4.alive(), "tool-level error does not mark server down");

    // restart() clears the down state and re-handshakes on next use
    client3.restart();
    check(client3.alive(), "restart clears down flag");
}

void test_registry_wires_mcp_tool() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
        {},
        {reply_to(2, R"({"tools":[{"name":"now","description":"get time"}]})")},
    };
    ToolRegistry registry;
    registry.add(std::make_unique<McpClient>(
        server_config("time"), [&slot](const McpServerConfig&) { return std::move(slot); }));
    const auto tools_json = registry.to_openai_tools_json();
    check(tools_json.find("time__now") != std::string::npos &&
              tools_json.find("get time") != std::string::npos,
          "mcp tool reaches the openai tools json");
}

void test_stdio_transport_real_child() {
    using namespace mini_agent;
    // The gate runs from the repo root (scripts/cpp_check.py contract).
    const std::string script = "tests/mock_mcp_server.py";
    if (::access(script.c_str(), R_OK) != 0) {
        std::cout << "SKIP stdio E2E: tests/mock_mcp_server.py not found\n";
        return;
    }
    McpServerConfig config;
    config.name = "mock";
    config.transport = "stdio";
    config.command = "python3";
    config.args = {script};
    McpClient client(config, default_transport_factory(), std::chrono::seconds(20),
                     std::chrono::seconds(15));
    std::string error;
    if (!client.ensure_started(&error)) {
        // python3 missing is an environment condition, not a product bug.
        std::cout << "SKIP stdio E2E: " << error << "\n";
        return;
    }
    auto defs = client.list();
    check(defs.size() == 2, "mock server lists two tools");
    const bool has_cat = std::any_of(defs.begin(), defs.end(), [](const ToolDefinition& d) {
        return d.name == "mock__cat_text_file";
    });
    check(has_cat, "namespaced tool name present");
    const auto result =
        client.call(ToolCall{"t1", "mock__echo", R"({"text":"hello mcp"})"}, [] { return false; });
    check(!result.is_error && result.text.find("hello mcp") != std::string::npos,
          "stdio E2E tools/call works");
    const auto failed = client.call(ToolCall{"t2", "mock__echo", "{}"}, [] { return false; });
    check(failed.is_error, "missing argument -> isError propagated");
    client.restart(); // deterministic child teardown before process exit
}

void test_stdio_transport_dies_clean() {
    using namespace mini_agent;
    McpServerConfig config;
    config.name = "dead";
    config.transport = "stdio";
    config.command = "python3";
    config.args = {"-c", "import sys; sys.exit(3)"};
    McpClient client(config, default_transport_factory(), std::chrono::seconds(10),
                     std::chrono::seconds(5));
    auto defs = client.list();
    check(defs.empty() && !client.alive(), "immediately-exiting child marks client down (no hang)");
}

void test_http_transport_round_trip() {
    using namespace mini_agent;
    const std::string script = "tests/mock_mcp_http_server.py";
    if (::access(script.c_str(), R_OK) != 0) {
        std::cout << "SKIP http E2E: script not found\n";
        return;
    }
    // Spawn the mock server on a fixed loopback port; wait via retries.
    const std::string port = "18933";
    pid_t server_pid = -1;
    {
        int null_fd = ::open("/dev/null", O_WRONLY);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
            ::setpgid(0, 0);
            const char* const argv[] = {const_cast<char*>("python3"),
                                        const_cast<char*>(script.c_str()),
                                        const_cast<char*>(port.c_str()), nullptr};
            ::execvp("python3", const_cast<char* const*>(argv));
            ::_exit(127);
        }
        server_pid = pid;
    }
    struct Guard {
        pid_t pid;
        ~Guard() {
            if (pid > 0) {
                ::kill(-pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
        }
    } guard{server_pid};

    McpServerConfig config;
    config.name = "http";
    config.transport = "http";
    config.url = "http://127.0.0.1:" + port + "/mcp";
    McpClient client(config, default_transport_factory(), std::chrono::seconds(3),
                     std::chrono::seconds(10));

    // Server boot race: retry handshake a few times (client restarts
    // itself; each attempt is bounded by the 3 s handshake timeout).
    bool up = false;
    std::vector<ToolDefinition> defs;
    for (int attempt = 0; attempt < 20 && !up; ++attempt) {
        client.restart();
        defs = client.list();
        up = client.alive() && !defs.empty();
        if (!up) {
            struct timespec nap{0, 100000000}; // 100 ms
            ::nanosleep(&nap, nullptr);
        }
    }
    check(up, "http transport E2E: handshake + tools/list via SSE response");
    if (!up)
        return;
    check(client.protocol_version() == "2025-06-18", "version recorded (echoes ours)");
    const auto result =
        client.call(ToolCall{"t1", "http__echo", R"({"text":"over http"})"}, [] { return false; });
    check(!result.is_error && result.text.find("over http") != std::string::npos,
          "http tools/call works (session header round trip)");
    const auto failed = client.call(ToolCall{"t2", "http__fail", "{}"}, [] { return false; });
    check(failed.is_error && failed.text == "boom", "http isError path");
    check(client.alive(), "tool-level error keeps http server alive");
}

void test_call_restart_after_child_death() {
    using namespace mini_agent;
    // v4.17: a transport that was UP and died mid-request gets one
    // transparent restart + re-handshake + retry. The factory hands out a
    // fresh fake per spawn; the first fake dies right after the call is
    // sent (request never answered -> no side-effect risk -> retry safe).
    std::vector<FakeTransport*> spawned;
    auto factory = [&spawned](const McpServerConfig&) {
        auto fake = std::make_unique<FakeTransport>();
        auto* raw = fake.get();
        spawned.push_back(raw);
        if (spawned.size() == 1) {
            // send 1 = initialize, send 2 = initialized, send 3 = the
            // tools/call: the channel dies with no reply (id stays 2 on
            // the respawned server, which restart() re-handshakes fresh).
            raw->reply_script_ = {
                {reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
                {},
            };
            raw->die_after_sends_ = 3;
        } else {
            raw->reply_script_ = {
                {reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
                {},
                {reply_to(2, R"({"content":[{"type":"text","text":"recovered"}]})")},
            };
        }
        return fake;
    };
    McpClient client(server_config("phx"), factory);
    const auto result =
        client.call(ToolCall{"t1", "phx__echo", R"({"x":1})"}, [] { return false; });
    check(!result.is_error && result.text == "recovered",
          "call auto-restarts a dead transport and retries once");
    check(spawned.size() == 2, "recovery spawned exactly one fresh transport");
    check(client.alive(), "client stays alive after successful recovery");

    // Third spawn also dead -> single retry budget, plain error, no loop.
    auto factory2 = [&spawned](const McpServerConfig&) {
        auto fake = std::make_unique<FakeTransport>();
        auto* raw = fake.get();
        spawned.push_back(raw);
        raw->reply_script_ = {
            {reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
            {},
        };
        raw->die_after_sends_ = 3; // dies on both attempts
        return fake;
    };
    const std::size_t before = spawned.size();
    McpClient client2(server_config("phx"), factory2);
    const auto failed = client2.call(ToolCall{"t2", "phx__echo", "{}"}, [] { return false; });
    check(failed.is_error && failed.text.find("closed") != std::string::npos,
          "repeated transport death surfaces an error after one retry");
    check(spawned.size() == before + 2, "retry budget is exactly one restart");
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const int only = argc > 1 ? std::atoi(argv[1]) : 0;
    if (only == 0 || only == 1)
        test_handshake_and_tool_round_trip();
    if (only == 0 || only == 2)
        test_foreign_and_error_messages();
    if (only == 0 || only == 3)
        test_registry_wires_mcp_tool();
    if (only == 0 || only == 4)
        test_stdio_transport_real_child();
    if (only == 0 || only == 5)
        test_stdio_transport_dies_clean();
    if (only == 0 || only == 6)
        test_http_transport_round_trip();
    if (only == 0 || only == 7)
        test_call_restart_after_child_death();
    if (failures) {
        std::cerr << failures << " mcp check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all mcp checks passed\n";
    return EXIT_SUCCESS;
}
