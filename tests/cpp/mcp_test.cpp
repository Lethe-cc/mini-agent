// Unit + transport tests for the v2 MCP client (design 008 section 8):
// RPC core over a scripted fake transport, McpClient dialogue rules, and
// a real-child-process stdio round trip against tests/mock_mcp_server.py
// (skipped when python3 or the script is absent). Plain asserts, no
// framework dependency, mirroring agent_test.cpp.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
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
    // Empty factory = production routing: MINI_AGENT_MCP_BACKEND selects
    // the session backend, so CTest runs cover legacy AND cxxmcp here.
    McpClient client(config, {}, std::chrono::seconds(20),
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
    McpClient client(config, {}, std::chrono::seconds(10),
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
    McpClient client(config, {}, std::chrono::seconds(3),
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

// Phase B erratum regression (design 021 §5.1): the backend switch must
// engage for production routing (empty factory). An inline stdio server
// records every client message. The stdio wire is identical by design,
// so the discriminator is the request id sequence: legacy ids are
// strictly monotonic across handshake + tools/list (1,2), while the
// cxxmcp session restarts its low-level counter after Client::initialize
// (tools/list carries id 1). A non-null factory (test fakes included)
// must keep forcing legacy regardless of the env.
void test_backend_switch_selection() {
    using namespace mini_agent;
    const char* script =
        "import json,sys\n"
        "log=open(sys.argv[1],'w')\n"
        "for line in sys.stdin:\n"
        "    try: m=json.loads(line)\n"
        "    except Exception: continue\n"
        "    log.write(json.dumps(m)+'\\n');log.flush()\n"
        "    rid=m.get('id');method=m.get('method')\n"
        "    if rid is None: continue\n"
        "    if method=='initialize':\n"
        "        r={'protocolVersion':'2025-06-18','capabilities':{'tools':{}},'serverInfo':{'name':'probe','version':'0'}}\n"
        "    elif method=='tools/list': r={'tools':[]}\n"
        "    else: r={}\n"
        "    sys.stdout.write(json.dumps({'jsonrpc':'2.0','id':rid,'result':r})+'\\n')\n"
        "    sys.stdout.flush()\n";
    char path[] = "/tmp/mcp_backend_probe_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        std::cout << "SKIP backend switch probe: mkstemp failed\n";
        return;
    }
    ::close(fd);
    struct PathGuard {
        const char* path;
        ~PathGuard() {
            ::unlink(path);
        }
    } guard{path};

    McpServerConfig config;
    config.name = "probe";
    config.transport = "stdio";
    config.command = "python3";
    config.args = {"-c", script, path};

    // Extract the numeric id of the tools/list request from the log
    // (python json.dumps writes `"id": N`; tolerate absent spacing).
    auto tools_list_id = [&path]() -> long long {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("tools/list") == std::string::npos)
                continue;
            auto pos = line.find("\"id\":");
            if (pos == std::string::npos)
                continue;
            pos += 5;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == ':'))
                ++pos;
            return std::strtoll(line.c_str() + pos, nullptr, 10);
        }
        return -1;
    };

    ::setenv("MINI_AGENT_MCP_BACKEND", "cxxmcp", 1);
    {
        McpClient client(config, {}, std::chrono::seconds(10), std::chrono::seconds(5));
        std::string error;
        if (!client.ensure_started(&error)) {
            // python3 missing is an environment condition, not a bug.
            std::cout << "SKIP backend switch probe: " << error << "\n";
            ::unsetenv("MINI_AGENT_MCP_BACKEND");
            return;
        }
        (void)client.list();
        client.restart(); // flush + tear down the child before reading
        check(tools_list_id() == 1,
              "empty factory + env=cxxmcp engages cxxmcp (SDK id counter restarts)");
    }
    {
        // Injected factory forces legacy even with env=cxxmcp.
        McpClient client(config, default_transport_factory(), std::chrono::seconds(10),
                         std::chrono::seconds(5));
        std::string error;
        check(client.ensure_started(&error), "injected default factory still starts");
        (void)client.list();
        client.restart();
        check(tools_list_id() == 2,
              "non-null factory bypasses the switch (legacy monotonic ids)");
    }
    ::unsetenv("MINI_AGENT_MCP_BACKEND");
    {
        McpClient client(config, {}, std::chrono::seconds(10), std::chrono::seconds(5));
        std::string error;
        check(client.ensure_started(&error), "empty factory default backend starts");
        (void)client.list();
        client.restart();
        check(tools_list_id() == 1,
              "no env defaults to cxxmcp since the M1 flip (SDK ids)");
    }
    ::setenv("MINI_AGENT_MCP_BACKEND", "legacy", 1);
    {
        McpClient client(config, {}, std::chrono::seconds(10), std::chrono::seconds(5));
        std::string error;
        check(client.ensure_started(&error), "env=legacy rollback starts");
        (void)client.list();
        client.restart();
        check(tools_list_id() == 2,
              "env=legacy escapes back to the self-written path (monotonic ids)");
    }
    ::unsetenv("MINI_AGENT_MCP_BACKEND");
}

// ---------------------------------------------------------------------------
// M2a resource/prompt seam (design 022 §4 T1–T4, §5.1)
// ---------------------------------------------------------------------------

const char* kRichCaps =
    R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{},"resources":{"listChanged":true},"prompts":{"listChanged":true}}})";

bool has_def(const std::vector<mini_agent::ToolDefinition>& defs, const std::string& name) {
    return std::any_of(defs.begin(), defs.end(),
                       [&](const mini_agent::ToolDefinition& d) { return d.name == name; });
}

// T1: capability gating -- no resources/prompts capabilities means no
// synthetic tools and folded errors on the seam.
void test_m2a_capability_gating() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {reply_to(1, R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}}})")},
        {},
        {reply_to(2, R"({"tools":[{"name":"t"}]})")},
    };
    McpClient client(server_config("plain"),
                     [&slot](const McpServerConfig&) { return std::move(slot); });
    const auto defs = client.list();
    check(defs.size() == 1 && defs[0].name == "plain__t",
          "no capability -> no synthetic tools");
    check(!client.has_resources() && !client.has_prompts(),
          "capability probes stay false without advertisement");
    std::string error;
    check(client.list_resources(&error).empty() &&
              error == "server does not support resources",
          "resources listing folds capability error");
    error.clear();
    check(client.list_prompts(&error).empty() && error == "server does not support prompts",
          "prompts listing folds capability error");
    error.clear();
    check(client.read_resource("mock://x", &error).empty() &&
              error == "server does not support resources",
          "read_resource gated on capability");
    error.clear();
    check(client.get_prompt("p", "{}", &error).empty() &&
              error == "server does not support prompts",
          "get_prompt gated on capability");
    check(client.alive(), "capability errors do not mark the server down");
}

// T1/T3: synthetic tools ride list() and call(); dispatch never reaches
// the remote tools/call path.
void test_m2a_synthetic_dispatch() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {reply_to(1, kRichCaps)},
        {},
        {reply_to(2, R"({"tools":[{"name":"t"}]})")},
        {reply_to(3, R"({"resources":[{"uri":"mock://a","name":"a"}]})")}, // list via call
        {reply_to(4, R"({"contents":[{"uri":"mock://a","text":"body"}]})")}, // read
        {reply_to(5, R"({"prompts":[{"name":"p"}]})")}, // prompts list via call
        {reply_to(6, R"({"messages":[{"role":"user","content":{"type":"text","text":"hi"}}]})")},
        {reply_to(7, R"({"contents":[{"uri":"mock://a","text":"body"}]})")}, // read via seam
    };
    McpClient client(server_config("srv"),
                     [&slot](const McpServerConfig&) { return std::move(slot); });
    const auto defs = client.list();
    check(defs.size() == 3 && has_def(defs, "srv__mcp_resources") &&
              has_def(defs, "srv__mcp_prompts"),
          "synthetic tools appended when both capabilities advertised");
    check(client.has_resources() && client.has_prompts(), "capability probes true");
    for (const auto& d : defs) {
        if (d.name.find("__mcp_") != std::string::npos)
            check(d.source == "srv" && !d.input_schema_json.empty() &&
                      d.input_schema_json.find("\"action\"") != std::string::npos,
                  "synthetic definition carries source + action schema");
    }
    const auto listed = client.call(
        ToolCall{"a", "srv__mcp_resources", R"({"action":"list"})"}, [] { return false; });
    check(!listed.is_error && listed.text.find("mock://a") != std::string::npos,
          "synthetic action=list returns the metadata array");
    check(fake->sent_.back().find("resources/list") != std::string::npos &&
              fake->sent_.back().find("tools/call") == std::string::npos,
          "synthetic dispatch never reaches remote tools/call");
    const auto read = client.call(
        ToolCall{"b", "srv__mcp_resources", R"({"action":"read","uri":"mock://a"})"},
        [] { return false; });
    check(!read.is_error && read.text.find("body") != std::string::npos,
          "synthetic action=read returns the contents array");
    const auto prompts = client.call(
        ToolCall{"c", "srv__mcp_prompts", R"({"action":"list"})"}, [] { return false; });
    check(!prompts.is_error && prompts.text.find("\"p\"") != std::string::npos,
          "synthetic prompts action=list works");
    const auto got = client.call(
        ToolCall{"d", "srv__mcp_prompts", R"({"action":"get","name":"p"})"},
        [] { return false; });
    check(!got.is_error && got.text.find("hi") != std::string::npos,
          "synthetic prompts action=get renders the raw result");
    check(fake->sent_.back().find(R"("name":"p")") != std::string::npos &&
              fake->sent_.back().find(R"("arguments":{})") != std::string::npos,
          "get forwards name + empty arguments object");
    // Bad shapes fold into ToolResult errors without any wire traffic.
    const auto before = fake->sent_.size();
    const auto bad_args = client.call(ToolCall{"e", "srv__mcp_resources", "not json"},
                                      [] { return false; });
    check(bad_args.is_error, "non-object arguments fold");
    const auto no_action = client.call(ToolCall{"f", "srv__mcp_resources", "{}"},
                                       [] { return false; });
    check(no_action.is_error && no_action.text.find("action=list|read") != std::string::npos,
          "missing action folds with usage hint");
    const auto no_uri =
        client.call(ToolCall{"g", "srv__mcp_resources", R"({"action":"read"})"},
                    [] { return false; });
    check(no_uri.is_error && no_uri.text.find("uri") != std::string::npos,
          "read without uri folds");
    const auto bad_action =
        client.call(ToolCall{"h", "srv__mcp_prompts", R"({"action":"read"})"},
                    [] { return false; });
    check(bad_action.is_error && bad_action.text.find("list|get") != std::string::npos,
          "prompts rejects resources actions");
    check(fake->sent_.size() == before, "argument validation never hits the wire");
    check(client.alive(), "synthetic argument errors keep the server alive");
}

// T2: metadata cache hits, contents never cached, list_changed marks
// stale (atomic-only observer), restart resets.
void test_m2a_cache_and_list_changed() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    const char* notification =
        R"({"jsonrpc":"2.0","method":"notifications/resources/list_changed"})";
    fake->reply_script_ = {
        {reply_to(1, kRichCaps)},
        {},
        {reply_to(2, R"({"tools":[]})")},
        {reply_to(3, R"({"resources":[{"uri":"mock://a"}]})")},
        {reply_to(4, R"({"contents":[{"uri":"mock://a","text":"one"}]})")},
        // The notification piggybacks on the second read's reply batch:
        // the legacy request loop dispatches it inline before matching id 5.
        {notification, reply_to(5, R"({"contents":[{"uri":"mock://a","text":"two"}]})")},
        {reply_to(6, R"({"resources":[{"uri":"mock://a"},{"uri":"mock://b"}]})")},
        {R"({"jsonrpc":"2.0","method":"notifications/prompts/list_changed"})",
         reply_to(7, R"({"prompts":[{"name":"p"}]})")},
        {reply_to(8, R"({"prompts":[{"name":"p"},{"name":"q"}]})")},
    };
    McpClient client(server_config("cache"),
                     [&slot](const McpServerConfig&) { return std::move(slot); });
    check(client.list().size() == 2, "only synthetics listed (tools empty)");
    std::string error;
    const auto first = client.list_resources(&error);
    check(error.empty() && first.find("mock://a") != std::string::npos, "first listing ok");
    const auto before = fake->sent_.size();
    const auto second = client.list_resources(&error);
    check(second == first && fake->sent_.size() == before,
          "metadata cache hit sends nothing (T2)");
    // Contents are NEVER cached: two reads = two round trips, fresh text.
    const auto one = client.read_resource("mock://a", &error);
    const auto two = client.read_resource("mock://a", &error);
    check(one.find("one") != std::string::npos && two.find("two") != std::string::npos &&
              fake->sent_.size() == before + 2,
          "read_resource always goes to the wire (T2 no content cache)");
    // Id-less list_changed piggybacks on the next reply; observer marks
    // stale; the following listing re-fetches and sees the new entry.
    const auto third = client.list_resources(&error);
    check(third.find("mock://b") != std::string::npos && fake->sent_.size() == before + 3,
          "resources list_changed invalidates the metadata cache");
    const auto p1 = client.list_prompts(&error);
    const auto p2 = client.list_prompts(&error);
    check(p1.find("\"q\"") == std::string::npos && p2.find("\"q\"") != std::string::npos,
          "prompts list_changed invalidates independently");
    // restart clears caches; next listing walks again.
    client.restart();
    check(!client.initialized() && client.alive(), "restart resets state");
}

// T1: malformed seam results fold precisely; read/get malformed results
// are errors WITHOUT marking the server down (spec-shape errors, not
// transport death); malformed list results DO mark down (desync risk).
void test_m2a_error_folding() {
    using namespace mini_agent;
    {
        std::unique_ptr<McpTransport> slot;
        auto* fake = install_fake(slot);
        fake->reply_script_ = {
            {reply_to(1, kRichCaps)},
            {},
            {reply_to(2, "[]")}, // resources/list result not an object
        };
        McpClient client(server_config("mal"),
                         [&slot](const McpServerConfig&) { return std::move(slot); });
        std::string error;
        check(client.list_resources(&error).empty() &&
                  error == "resources/list result is not an object",
              "non-object list result folds with method name");
        check(!client.alive(), "malformed list result marks the client down");
    }
    {
        std::unique_ptr<McpTransport> slot;
        auto* fake = install_fake(slot);
        fake->reply_script_ = {
            {reply_to(1, kRichCaps)},
            {},
            {reply_to(2, R"({"items":[]})")}, // missing resources array
        };
        McpClient client(server_config("mal"),
                         [&slot](const McpServerConfig&) { return std::move(slot); });
        std::string error;
        check(client.list_resources(&error).empty() &&
                  error == "resources/list missing resources array",
              "missing item array folds with key name");
        check(!client.alive(), "missing item array marks down");
    }
    {
        std::unique_ptr<McpTransport> slot;
        auto* fake = install_fake(slot);
        fake->reply_script_ = {
            {reply_to(1, kRichCaps)},
            {},
            {reply_to(2, R"({"contents":[{"text":"ok"}]})")},
            {reply_to(3, R"({"nope":1})")}, // missing contents array
            {reply_to(4, R"({"messages":[]})")},
        };
        McpClient client(server_config("shape"),
                         [&slot](const McpServerConfig&) { return std::move(slot); });
        std::string error;
        check(client.read_resource("", &error).empty() &&
                  error == "read_resource requires a uri",
              "empty uri folds before the wire");
        error.clear(); // seam only writes *error on failure
        const auto ok = client.read_resource("mock://a", &error);
        check(error.empty() && ok.find("ok") != std::string::npos, "read round trip");
        check(client.read_resource("mock://a", &error).empty() &&
                  error == "resources/read missing contents array",
              "malformed read folds without down");
        check(client.alive(), "read shape error keeps the server alive");
        error.clear();
        check(client.get_prompt("", "{}", &error).empty() &&
                  error == "get_prompt requires a name",
              "empty prompt name folds");
        error.clear();
        check(client.get_prompt("p", "[1]", &error).empty() &&
                  error == "prompt arguments must be a JSON object",
              "non-object prompt arguments fold");
        error.clear();
        const auto got = client.get_prompt("p", "", &error);
        check(error.empty() && got.find("messages") != std::string::npos,
              "empty args_json accepted as no arguments");
        check(client.alive(), "prompt shape errors keep the server alive");
    }
    {
        // MRTR resultType=input_required folds as an error (M2a stays at
        // 2025-06-18; multi-round prompts arrive with the version bump).
        std::unique_ptr<McpTransport> slot;
        auto* fake = install_fake(slot);
        fake->reply_script_ = {
            {reply_to(1, kRichCaps)},
            {},
            {reply_to(2, R"({"resultType":"input_required"})")},
        };
        McpClient client(server_config("mrtr"),
                         [&slot](const McpServerConfig&) { return std::move(slot); });
        std::string error;
        check(client.get_prompt("p", "{}", &error).empty() &&
                  error.find("additional input") != std::string::npos,
              "input_required folds as unsupported multi-round prompt");
        check(client.alive(), "MRTR fold keeps the server alive");
    }
}

// T1: the bounded 16-page walk applies to resources/prompts listings too.
void test_m2a_page_cap() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {reply_to(1, kRichCaps)},
        {},
    };
    // 17 pages, each pointing at the next: the walk must stop after 16.
    for (int page = 0; page < 17; ++page)
        fake->reply_script_.push_back(
            {reply_to(2 + page,
                      R"({"resources":[{"uri":"mock://p)" + std::to_string(page) +
                          R"("}],"nextCursor":"n)" + std::to_string(page + 1) + "\"}")});
    McpClient client(server_config("pages"),
                     [&slot](const McpServerConfig&) { return std::move(slot); });
    std::string error;
    const auto listed = client.list_resources(&error);
    check(error.empty(), "page-capped listing succeeds");
    check(listed.find("mock://p15") != std::string::npos &&
              listed.find("mock://p16") == std::string::npos,
          "resources walk stops at the 16-page cap");
    check(fake->sent_.size() == 2 + 16, "exactly 16 listing requests sent");
    check(client.alive(), "page cap is a bound, not a failure");
}

// T3 (registry seam): find_by_source is PASSIVE -- it must not start an
// MCP client (lazy contract 008 §5.5) nor require a prior list().
void test_m2a_registry_passive_lookup() {
    using namespace mini_agent;
    std::unique_ptr<McpTransport> slot;
    auto* fake = install_fake(slot);
    fake->reply_script_ = {
        {reply_to(1, kRichCaps)},
        {},
        {reply_to(2, R"({"resources":[{"uri":"mock://a"}]})")},
    };
    ToolRegistry registry;
    registry.add(std::make_unique<McpClient>(
        server_config("lazy"), [&slot](const McpServerConfig&) { return std::move(slot); }));
    auto* runtime = registry.find_by_source("lazy");
    check(runtime != nullptr, "passive lookup finds the runtime by source name");
    check(fake->sent_.empty(), "find_by_source sends nothing (no lazy start)");
    auto* client = dynamic_cast<McpClient*>(runtime);
    check(client && !client->initialized(), "client still unstarted after lookup");
    check(registry.find_by_source("nope") == nullptr, "unknown source yields nullptr");
    // Explicit listing starts ONLY this server (owner-thread contract).
    std::string error;
    const auto listed = client->list_resources(&error);
    check(error.empty() && listed.find("mock://a") != std::string::npos &&
              client->initialized(),
          "explicit listing lazily starts the named server");
}

// E2E: rich stdio mock (both backends via the env matrix). Pagination,
// read, prompt render, and piggybacked list_changed invalidation.
void test_stdio_rich_e2e() {
    using namespace mini_agent;
    const std::string script = "tests/mock_mcp_server.py";
    if (::access(script.c_str(), R_OK) != 0) {
        std::cout << "SKIP rich stdio E2E: script not found\n";
        return;
    }
    McpServerConfig config;
    config.name = "rich";
    config.transport = "stdio";
    config.command = "python3";
    config.args = {script, "--rich"};
    McpClient client(config, {}, std::chrono::seconds(20), std::chrono::seconds(15));
    std::string error;
    if (!client.ensure_started(&error)) {
        std::cout << "SKIP rich stdio E2E: " << error << "\n";
        return;
    }
    const auto defs = client.list();
    check(defs.size() == 5 && has_def(defs, "rich__mcp_resources") &&
              has_def(defs, "rich__mcp_prompts") && has_def(defs, "rich__notify"),
          "rich mock lists 3 tools + 2 synthetics");
    const auto listed = client.list_resources(&error);
    check(error.empty() && listed.find("mock://doc/a") != std::string::npos &&
              listed.find("mock://doc/c") != std::string::npos,
          "resources/list pagination aggregated across pages");
    const auto before = client.list_resources(&error);
    check(before == listed, "second listing served from cache");
    const auto body = client.read_resource("mock://doc/b", &error);
    check(error.empty() && body.find("beta resource body") != std::string::npos,
          "resources/read round trip");
    error.clear();
    check(client.read_resource("mock://missing", &error).empty() && !error.empty(),
          "unknown resource folds the JSON-RPC error");
    check(client.alive(), "resource-level error keeps the server alive");
    error.clear();
    const auto prompts = client.list_prompts(&error);
    check(error.empty() && prompts.find("review-code") != std::string::npos,
          "prompts/list round trip");
    const auto rendered =
        client.get_prompt("review-code", R"({"code":"x=1","lang":"python"})", &error);
    check(error.empty() && rendered.find("review this python code") != std::string::npos &&
              rendered.find("x=1") != std::string::npos,
          "prompts/get forwards arguments");
    check(rendered.find("\"assistant\"") != std::string::npos &&
              rendered.find("\"image\"") != std::string::npos,
          "raw result keeps assistant-role and image blocks for UI downgrade");
    // notify tool piggybacks list_changed before its result: caches must
    // re-fetch afterwards (both backends dispatch the notification).
    const auto notified =
        client.call(ToolCall{"n", "rich__notify", "{}"}, [] { return false; });
    check(!notified.is_error && notified.text == "notified", "notify tool call ok");
    const auto refreshed = client.list_resources(&error);
    check(error.empty() && !refreshed.empty(), "resources cache invalidated by notification");
    const auto refreshed_prompts = client.list_prompts(&error);
    check(error.empty() && !refreshed_prompts.empty(),
          "prompts cache invalidated by notification");
    client.restart(); // deterministic teardown
}

// E2E: rich HTTP mock on a second port. Exercises POST-SSE piggybacked
// notifications plus the SDK's standing GET stream (rich mode serves it).
void test_http_rich_e2e() {
    using namespace mini_agent;
    const std::string script = "tests/mock_mcp_http_server.py";
    if (::access(script.c_str(), R_OK) != 0) {
        std::cout << "SKIP rich http E2E: script not found\n";
        return;
    }
    const std::string port = "18934";
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
                                        const_cast<char*>(port.c_str()),
                                        const_cast<char*>("--rich"), nullptr};
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
    config.name = "httprich";
    config.transport = "http";
    config.url = "http://127.0.0.1:" + port + "/mcp";
    McpClient client(config, {}, std::chrono::seconds(3), std::chrono::seconds(10));
    bool up = false;
    for (int attempt = 0; attempt < 20 && !up; ++attempt) {
        client.restart();
        std::string error;
        up = client.ensure_started(&error);
        if (!up) {
            struct timespec nap{0, 100000000};
            ::nanosleep(&nap, nullptr);
        }
    }
    check(up, "rich http handshake");
    if (!up)
        return;
    const auto defs = client.list();
    check(has_def(defs, "httprich__mcp_resources") && has_def(defs, "httprich__mcp_prompts"),
          "http rich capabilities yield synthetic tools");
    std::string error;
    const auto listed = client.list_resources(&error);
    check(error.empty() && listed.find("mock://doc/c") != std::string::npos,
          "http resources/list pagination");
    const auto body = client.read_resource("mock://doc/a", &error);
    check(error.empty() && body.find("alpha resource body") != std::string::npos,
          "http resources/read");
    const auto rendered = client.get_prompt("review-code", R"({"code":"y=2"})", &error);
    check(error.empty() && rendered.find("y=2") != std::string::npos, "http prompts/get");
    // notify piggybacks list_changed on the POST SSE response; both
    // backends must dispatch it and invalidate the metadata caches.
    const auto notified =
        client.call(ToolCall{"n", "httprich__notify", "{}"}, [] { return false; });
    check(!notified.is_error && notified.text == "notified", "http notify tool ok");
    const auto refreshed = client.list_resources(&error);
    check(error.empty() && !refreshed.empty(), "http piggybacked notification invalidates");
    client.restart(); // DELETE + stream teardown before the guard kills python
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
    if (only == 0 || only == 8)
        test_backend_switch_selection();
    if (only == 0 || only == 9)
        test_m2a_capability_gating();
    if (only == 0 || only == 10)
        test_m2a_synthetic_dispatch();
    if (only == 0 || only == 11)
        test_m2a_cache_and_list_changed();
    if (only == 0 || only == 12)
        test_m2a_error_folding();
    if (only == 0 || only == 13)
        test_m2a_page_cap();
    if (only == 0 || only == 14)
        test_m2a_registry_passive_lookup();
    if (only == 0 || only == 15)
        test_stdio_rich_e2e();
    if (only == 0 || only == 16)
        test_http_rich_e2e();
    if (failures) {
        std::cerr << failures << " mcp check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all mcp checks passed\n";
    return EXIT_SUCCESS;
}
