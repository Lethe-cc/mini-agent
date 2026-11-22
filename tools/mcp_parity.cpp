// Backend parity probe (design 021 M1 phase C): drives the REAL McpClient
// facade against the repo's mock servers plus inline adversarial servers,
// printing one deterministic transcript line per observable (list/call/
// alive/error/protocol_version). Run the same binary under both backends
// and diff the transcripts byte-for-byte (tools/mcp_parity_check.py):
//
//   MINI_AGENT_MCP_BACKEND=legacy ./mcp_parity <scenario> > legacy.txt
//   MINI_AGENT_MCP_BACKEND=cxxmcp ./mcp_parity <scenario> > cxxmcp.txt
//   diff legacy.txt cxxmcp.txt
//
// Not a CTest target. Manual build (mirrors the mcp_test link line):
//   g++ -std=c++17 -g -Iinclude tools/mcp_parity.cpp -o /tmp/mcp_parity \
//       build/linux-gcc-debug/libagent_core.a \
//       build/linux-gcc-debug/third_party/cxxmcp/libmcp_client.a \
//       build/linux-gcc-debug/third_party/cxxmcp/libmcp_protocol.a \
//       -lcurl -lssl -lcrypto -pthread
//
// Scenarios (each self-contained; spawn their own mock where needed):
//   stdio-basic      mock_mcp_server.py: list/echo/isError/missing-arg/
//                    unknown-tool/rpc-error/abort paths, protocol_version
//   stdio-die        child exits immediately; list down; restart budget
//   stdio-spawnfail  nonexistent command: spawn/handshake failure text
//   stdio-timeout    child never answers tools/call: timeout text + down
//   http-basic       mock_mcp_http_server.py: handshake/tools/call/isError/
//                    rpc-error/DELETE close, protocol_version
//   http-401         inline server answers 401: auth-dead text
//   http-500         inline server answers 500: status text
//   http-404mid      session invalidated mid-run: legacy error vs SDK
//                    auto-recovery (KNOWN deviation, transcript records it)
//   http-nonstd      nonstandard JSON-RPC error code (KNOWN deviation)
//   http-noresult    reply without result/error: legacy "{}" OK vs SDK
//                    strict-parse error (KNOWN deviation)
#include "mini_agent/mcp.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace mini_agent;
using namespace std::chrono_literals;

namespace {

void line(const std::string& tag, const std::string& value) {
    std::cout << tag << ": " << value << '\n';
}

void show_tools(McpClient& client) {
    auto defs = client.list();
    line("list.count", std::to_string(defs.size()));
    for (const auto& d : defs)
        line("tool", d.name + " | " + d.description + " | " + d.source);
    line("alive", client.alive() ? "1" : "0");
}

void show_call(McpClient& client, const std::string& name, const std::string& args) {
    auto r = client.call(ToolCall{"t", name, args}, [] { return false; });
    line("call." + name, (r.is_error ? "ERR " : "OK ") + r.text);
    line("alive", client.alive() ? "1" : "0");
}

// --- mock process supervision (same pattern as mcp_test group 6) ---------
struct ChildGuard {
    pid_t pid = -1;
    ~ChildGuard() {
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

pid_t spawn_python(const char* script, const std::string& arg) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int null_fd = ::open("/dev/null", O_WRONLY);
        ::dup2(null_fd, STDERR_FILENO);
        ::close(null_fd);
        ::setpgid(0, 0);
        if (arg.empty()) {
            ::execlp("python3", "python3", script, nullptr);
        } else {
            ::execlp("python3", "python3", script, arg.c_str(), nullptr);
        }
        ::_exit(127);
    }
    return pid;
}

std::string http_reply(int status, const std::string& status_text, const std::string& body,
                       const std::string& extra_headers = {},
                       const std::string& ctype = "application/json") {
    return "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n" +
           "Content-Type: " + ctype + "\r\n" + "Content-Length: " + std::to_string(body.size()) +
           "\r\n" + extra_headers + "Connection: close\r\n\r\n" + body;
}

const char* kInitResult =
    R"("result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{}},)"
    R"("serverInfo":{"name":"tiny","version":"0"}})";
const char* kToolsResult =
    R"("result":{"tools":[{"name":"echo","description":"Echo text",)"
    R"("inputSchema":{"type":"object"}}]})";
const char* kEchoResult =
    R"("result":{"content":[{"type":"text","text":"recovered"}]})";

// --- inline scripted HTTP endpoint ----------------------------------------
// A minimal stateful MCP endpoint: initialize (new session each time),
// notifications/initialized (202), tools/list, tools/call routed through a
// per-call script. Both backends' request-id sequences differ after
// recovery, so replies echo the request's id instead of canned literals.
// The endpoint stops accepting once the script is exhausted; unconsumed
// canned entries are fine (legacy stops earlier than the SDK on 404).
struct TinyHttp {
    int fd = -1;
    int port = 0;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<int> served{0};
    int script_limit = 0;         // max requests to serve
    std::vector<std::string> call_bodies; // per tools/call, in order
    std::vector<int> call_statuses;       // HTTP status per tools/call
    std::size_t call_index = 0;
    std::string session;          // current valid session ("" = none)
    int session_seq = 0;
    bool reject_auth = false;
    int fail_status = 0;          // non-zero: fail every request with it

    TinyHttp() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        ::listen(fd, 8);
        worker = std::thread([this] { serve(); });
    }
    ~TinyHttp() {
        stopping = true;
        if (fd >= 0)
            ::close(fd);
        if (worker.joinable())
            worker.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    }

    void serve() {
        while (!stopping && served < script_limit) {
            // Poll so teardown never blocks on an accept that close()
            // alone would not wake.
            pollfd p{fd, POLLIN, 0};
            const int ready = ::poll(&p, 1, 100);
            if (ready < 0)
                return;
            if (ready == 0)
                continue;
            const int conn = ::accept(fd, nullptr, nullptr);
            if (conn < 0)
                return;
            handle(conn);
            ::close(conn);
            ++served;
        }
    }

    void handle(int conn) {
        // Read one request: headers, then Content-Length body.
        std::string req;
        char buf[4096];
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0)
                return;
            req.append(buf, static_cast<std::size_t>(n));
            header_end = req.find("\r\n\r\n");
        }
        std::size_t body_len = 0;
        const auto cl = req.find("Content-Length:");
        if (cl != std::string::npos)
            body_len = static_cast<std::size_t>(std::strtoul(req.c_str() + cl + 15, nullptr, 10));
        while (req.size() < header_end + 4 + body_len) {
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0)
                return;
            req.append(buf, static_cast<std::size_t>(n));
        }
        const std::string body = req.substr(header_end + 4, body_len);
        if (std::getenv("PARITY_TRACE")) {
            std::string sess_hdr;
            const auto sp = req.find("Mcp-Session-Id:");
            if (sp != std::string::npos)
                sess_hdr = req.substr(sp, req.find("\r\n", sp) - sp);
            std::cerr << "[trace] " << req.substr(0, req.find("\r\n"))
                      << " | " << sess_hdr << " | body: " << body << '\n';
        }
        std::string reply;
        if (fail_status != 0) {
            reply = http_reply(fail_status, "Error", "boom");
        } else if (req.compare(0, 4, "POST") != 0) {
            reply = http_reply(405, "Method Not Allowed", "");
        } else if (body.find("\"method\":\"initialize\"") != std::string::npos) {
            session = "s" + std::to_string(++session_seq);
            reply = http_reply(200, "OK", with_id(body, kInitResult),
                               "Mcp-Session-Id: " + session + "\r\n");
        } else if (body.find("\"method\":\"notifications/initialized\"") != std::string::npos) {
            reply = http_reply(202, "Accepted", "");
        } else if (!session.empty() && req.find("Mcp-Session-Id: " + session) == std::string::npos) {
            session.clear(); // stale/absent session: expire it, spec 404 rule
            reply = http_reply(404, "Not Found", "unknown session");
        } else if (body.find("\"method\":\"tools/list\"") != std::string::npos) {
            reply = http_reply(200, "OK", with_id(body, kToolsResult));
        } else if (body.find("\"method\":\"tools/call\"") != std::string::npos) {
            if (call_index < call_bodies.size()) {
                const int status = call_index < call_statuses.size() ? call_statuses[call_index] : 200;
                const std::string& canned = call_bodies[call_index];
                ++call_index;
                if (status == 404)
                    session.clear();
                reply = http_reply(status, status == 200 ? "OK" : "Not Found",
                                   status == 200 ? with_id(body, canned) : canned);
            } else {
                reply = http_reply(200, "OK", with_id(body, kEchoResult));
            }
        } else {
            reply = http_reply(200, "OK",
                               with_id(body, R"("error":{"code":-32601,"message":"method not found"})"));
        }
        ::send(conn, reply.data(), reply.size(), MSG_NOSIGNAL);
    }

    // Echo the request's JSON-RPC id into a canned result/error fragment.
    static std::string with_id(const std::string& body, const std::string& fragment) {
        const auto pos = body.find("\"id\":");
        std::string id = "1";
        if (pos != std::string::npos) {
            const auto end = body.find_first_of(",}", pos + 5);
            id = body.substr(pos + 5, end - pos - 5);
        }
        return R"({"jsonrpc":"2.0","id":)" + id + "," + fragment + "}";
    }
};

McpServerConfig http_config(const std::string& url, const std::string& name = "tiny") {
    McpServerConfig config;
    config.name = name;
    config.transport = "http";
    config.url = url;
    return config;
}

void wait_boot(McpClient& client) {
    // Same boot-race retry as mcp_test: bounded handshake + restarts.
    for (int attempt = 0; attempt < 20; ++attempt) {
        client.restart();
        if (client.alive() && !client.list().empty())
            return;
        ::usleep(100000);
    }
}

// --- scenarios ------------------------------------------------------------

void scenario_stdio_basic() {
    McpServerConfig config;
    config.name = "mock";
    config.command = "python3";
    config.args = {"tests/mock_mcp_server.py"};
    McpClient client(config, {}, 3s, 5s);
    show_tools(client);
    line("version", client.protocol_version());
    show_call(client, "mock__echo", R"({"text":"hello parity"})");
    show_call(client, "mock__echo", R"({})"); // missing required arg -> isError
    show_call(client, "mock__echo", "not-json"); // bad args -> {} arguments
    show_call(client, "mock__nope", "{}"); // unknown tool -> rpc error text
    auto aborted =
        client.call(ToolCall{"t", "mock__echo", "{}"}, [] { return true; });
    line("abort", (aborted.is_error ? "ERR " : "OK ") + aborted.text);
    auto foreign = client.call(ToolCall{"t", "other__echo", "{}"}, [] { return false; });
    line("foreign", (foreign.is_error ? "ERR " : "OK ") + foreign.text);
}

void scenario_stdio_die() {
    McpServerConfig config;
    config.name = "dead";
    config.command = "python3";
    config.args = {"-c", "import sys; sys.exit(3)"};
    McpClient client(config, {}, 3s, 5s);
    show_tools(client);
    show_call(client, "dead__echo", "{}");
    show_call(client, "dead__echo", "{}"); // already down: no respawn per call
}

void scenario_stdio_spawnfail() {
    McpServerConfig config;
    config.name = "ghost";
    config.command = "/nonexistent/mcp-server-binary";
    McpClient client(config, {}, 3s, 5s);
    show_tools(client);
    show_call(client, "ghost__echo", "{}");
}

void scenario_stdio_timeout() {
    // Child completes the handshake but never answers tools/call.
    McpServerConfig config;
    config.name = "slow";
    config.command = "python3";
    config.args = {"-c",
                   "import json,sys,time\n"
                   "for l in sys.stdin:\n"
                   " m=json.loads(l)\n"
                   " if m.get('method')=='initialize':\n"
                   "  sys.stdout.write(json.dumps({'jsonrpc':'2.0','id':m['id'],'result':"
                   "{'protocolVersion':'2025-06-18','capabilities':{'tools':{}},"
                   "'serverInfo':{'name':'slow','version':'0'}}})+'\\n')\n"
                   "  sys.stdout.flush()\n"
                   " elif m.get('method')=='tools/list':\n"
                   "  sys.stdout.write(json.dumps({'jsonrpc':'2.0','id':m['id'],'result':"
                   "{'tools':[{'name':'echo','description':'Echo text','inputSchema':"
                   "{'type':'object'}}]}})+'\\n')\n"
                   "  sys.stdout.flush()\n"
                   " elif 'id' in m:\n"
                   "  time.sleep(30)\n"};
    McpClient client(config, {}, 3s, 1s);
    show_tools(client);
    show_call(client, "slow__echo", R"({"text":"x"})"); // 1s call timeout
    show_call(client, "slow__echo", R"({"text":"x"})"); // down, no retry on timeout
}

void scenario_http_basic() {
    ChildGuard guard;
    guard.pid = spawn_python("tests/mock_mcp_http_server.py", "18941");
    McpClient client(http_config("http://127.0.0.1:18941/mcp", "http"),
                     {}, 3s, 10s);
    wait_boot(client);
    show_tools(client);
    line("version", client.protocol_version());
    show_call(client, "http__echo", R"({"text":"over http"})");
    show_call(client, "http__fail", "{}");
    show_call(client, "http__nope", "{}"); // -32602 rpc error
    client.restart();
    show_tools(client); // fresh session after explicit restart
}

void scenario_http_401() {
    TinyHttp server;
    server.fail_status = 401;
    server.script_limit = 4;
    McpClient client(http_config(server.url()), {}, 3s, 5s);
    show_tools(client);
    show_call(client, "tiny__echo", "{}");
}

void scenario_http_500() {
    TinyHttp server;
    server.fail_status = 500;
    server.script_limit = 4;
    McpClient client(http_config(server.url()), {}, 3s, 5s);
    show_tools(client);
}

void scenario_http_404mid() {
    // initialize OK (session issued), tools/list OK, then the first
    // tools/call answers 404. VERIFIED deviation: legacy surfaces
    // "session expired" as an error; the SDK re-initializes transparently
    // and the retry succeeds. Later calls succeed on both.
    TinyHttp server;
    server.script_limit = 20;
    server.call_bodies = {"unknown session"};
    server.call_statuses = {404};
    McpClient client(http_config(server.url()), {}, 3s, 5s);
    show_tools(client);
    show_call(client, "tiny__echo", R"({"text":"x"})");
    line("note", "http-404mid: compare recovery behavior between backends");
    show_call(client, "tiny__echo", R"({"text":"y"})");
}

void scenario_http_nonstd() {
    // Nonstandard JSON-RPC error code outside the implementation-defined
    // reserved range (-32000..-32099): legacy prints "rpc error -32100",
    // the SDK map falls through to the raw message (KNOWN deviation).
    TinyHttp server;
    server.script_limit = 8;
    server.call_bodies = {R"("error":{"code":-32100,"message":"custom failure"})"};
    McpClient client(http_config(server.url()), {}, 3s, 5s);
    show_tools(client);
    show_call(client, "tiny__echo", "{}");
    line("deviation", "http-nonstd: rpc error code outside standard set (design 021 §9)");
}

void scenario_http_noresult() {
    // Reply with neither result nor error: legacy McpRpcClient yields
    // "{}" (empty OK text); the SDK rejects the envelope at serialization
    // ("Unsupported JSON-RPC entity") BEFORE the session's missing-result
    // mapping can fire (KNOWN deviation, design 021 §9 strict-parse).
    TinyHttp server;
    server.script_limit = 8;
    server.call_bodies = {R"("x":1)"}; // valid JSON-RPC envelope, no result/error
    McpClient client(http_config(server.url()), {}, 3s, 5s);
    show_tools(client);
    show_call(client, "tiny__echo", "{}");
    line("deviation", "http-noresult: legacy={} OK, cxxmcp=strict-parse error (design 021 §9)");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: mcp_parity <scenario>\n";
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    const std::string scenario = argv[1];
    if (scenario == "stdio-basic")
        scenario_stdio_basic();
    else if (scenario == "stdio-die")
        scenario_stdio_die();
    else if (scenario == "stdio-spawnfail")
        scenario_stdio_spawnfail();
    else if (scenario == "stdio-timeout")
        scenario_stdio_timeout();
    else if (scenario == "http-basic")
        scenario_http_basic();
    else if (scenario == "http-401")
        scenario_http_401();
    else if (scenario == "http-500")
        scenario_http_500();
    else if (scenario == "http-404mid")
        scenario_http_404mid();
    else if (scenario == "http-nonstd")
        scenario_http_nonstd();
    else if (scenario == "http-noresult")
        scenario_http_noresult();
    else {
        std::cerr << "unknown scenario: " << scenario << '\n';
        return 2;
    }
    return 0;
}