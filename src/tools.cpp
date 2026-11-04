#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace mini_agent {

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::add(std::unique_ptr<ToolRuntime> runtime) {
    if (!runtime)
        return;
    // Do NOT call runtime->list() here: that would defeat lazy start
    // (design 008 section 5.5) -- an MCP client spawns/handshakes inside
    // its first list(), so eagerly listing would hit the network on every
    // launch. Name claiming happens in list() (late-registered below),
    // and find() scans runtimes directly. Startup stays network-free.
    runtimes_.push_back(std::move(runtime));
}

std::vector<ToolDefinition> ToolRegistry::list() {
    std::vector<ToolDefinition> out;
    for (auto& runtime : runtimes_) {
        if (!runtime->alive())
            continue; // dead source: tools vanish, loop continues (008 5.5)
        for (auto& definition : runtime->list()) {
            if (std::find(names_.begin(), names_.end(), definition.name) == names_.end()) {
                names_.push_back(definition.name); // late-registered name
            }
            out.push_back(std::move(definition));
        }
    }
    return out;
}

ToolRuntime* ToolRegistry::find(const std::string& name) {
    for (auto& runtime : runtimes_) {
        if (!runtime->alive())
            continue;
        for (const auto& definition : runtime->list()) {
            if (definition.name == name)
                return runtime.get();
        }
    }
    return nullptr;
}

std::string ToolRegistry::to_openai_tools_json() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& definition : list()) {
        nlohmann::json entry;
        entry["type"] = "function";
        entry["function"]["name"] = definition.name;
        entry["function"]["description"] = definition.description;
        auto schema = nlohmann::json::parse(definition.input_schema_json, nullptr, false);
        entry["function"]["parameters"] =
            schema.is_discarded() ? nlohmann::json{{"type", "object"}} : std::move(schema);
        arr.push_back(std::move(entry));
    }
    return arr.dump();
}

// ---------------------------------------------------------------------------
// run_bash argument parsing (fail-closed)
// ---------------------------------------------------------------------------

std::optional<BashRequest> parse_bash_arguments(std::string_view arguments_json,
                                                std::size_t default_timeout_seconds,
                                                std::string* error) {
    auto parsed = nlohmann::json::parse(arguments_json, nullptr, false);
    if (parsed.is_discarded()) {
        if (error)
            *error = "tool arguments are not valid JSON";
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        if (error)
            *error = "tool arguments must be a JSON object";
        return std::nullopt;
    }
    BashRequest request;
    request.timeout_seconds = default_timeout_seconds;
    auto command_it = parsed.find("command");
    if (command_it == parsed.end() || !command_it->is_string() ||
        command_it->get_ref<const std::string&>().empty()) {
        if (error)
            *error = "run_bash requires a non-empty string 'command'";
        return std::nullopt;
    }
    request.command = command_it->get<std::string>();
    if (auto timeout_it = parsed.find("timeout"); timeout_it != parsed.end()) {
        if (!timeout_it->is_number_integer() || timeout_it->get<int64_t>() <= 0 ||
            timeout_it->get<int64_t>() > 3600) {
            if (error)
                *error = "run_bash 'timeout' must be an integer in 1..3600 seconds";
            return std::nullopt;
        }
        request.timeout_seconds = static_cast<std::size_t>(timeout_it->get<int64_t>());
    }
    return request;
}

std::string cap_tool_output(std::string_view text, std::size_t limit_bytes) {
    if (text.size() <= limit_bytes || limit_bytes < 64)
        return std::string(text);
    const std::string marker = "\n[... output truncated ...]\n";
    const std::size_t half = (limit_bytes - marker.size()) / 2;
    std::string out;
    out.append(text.substr(0, half));
    out += marker;
    out.append(text.substr(text.size() - half));
    return out;
}

std::string sanitize_display(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b && i + 1 < text.size()) {
            // Drop one ANSI escape sequence: ESC [ ... final-byte,
            // ESC ] ... BEL/ST, or ESC + single letter.
            const char next = text[i + 1];
            if (next == '[') {
                std::size_t j = i + 2;
                while (j < text.size() && !(text[j] >= 0x40 && text[j] <= 0x7e)) // CSI final byte
                    ++j;
                i = (j < text.size()) ? j + 1 : text.size();
                continue;
            }
            if (next == ']') {
                std::size_t j = i + 2;
                while (j < text.size() && text[j] != '\x07' && !(text[j] == 0x1b))
                    ++j;
                i = (j < text.size()) ? j + 1 : text.size();
                continue;
            }
            i += 2; // two-char escape
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            ++i; // drop other C0 controls
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// BuiltinRuntime: run_bash via fork/exec /bin/bash -lc
// ---------------------------------------------------------------------------

std::vector<ToolDefinition> BuiltinRuntime::list() {
    std::vector<ToolDefinition> out;
    ToolDefinition bash;
    bash.name = "run_bash";
    bash.description =
        "Run a shell command with /bin/bash -lc and return merged stdout+stderr. "
        "Use for inspection and command execution. Arguments: {\"command\": \"...\", "
        "\"timeout\": optional seconds}.";
    bash.input_schema_json =
        R"({"type":"object","properties":{"command":{"type":"string","description":"shell command"},"timeout":{"type":"integer","description":"max seconds, 1-3600"}},"required":["command"]})";
    bash.source = "builtin";
    out.push_back(std::move(bash));
    return out;
}

ToolResult BuiltinRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    if (call.name != "run_bash") {
        return ToolResult{"unknown builtin tool: " + call.name, true};
    }
    std::string arg_error;
    auto request = parse_bash_arguments(call.arguments_json, default_timeout_seconds_, &arg_error);
    if (!request) {
        return ToolResult{arg_error, true};
    }

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        return ToolResult{"pipe failed: " + std::string(std::strerror(errno)), true};
    }
    ::pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return ToolResult{"fork failed: " + std::string(std::strerror(errno)), true};
    }
    if (pid == 0) {
        // Child: new process group (so timeout can kill the whole tree),
        // stdout+stderr -> pipe write end, then exec with fixed argv.
        ::setpgid(0, 0);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        // Ensure fd 0 is valid for bash without clobbering a live stdin.
        if (::fcntl(0, F_GETFD) == -1) {
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull == 0) {
                // got the slot we wanted
            } else if (devnull > 0) {
                ::dup2(devnull, 0);
                ::close(devnull);
            }
        }
        const char* path = ::getenv("PATH") ? ::getenv("PATH") : "/usr/local/bin:/usr/bin:/bin";
        ::setenv("PATH", path, 1);
        ::setenv("LC_ALL", "C.UTF-8", 1);
        const char* const argv[] = {"/bin/bash", "-lc", request->command.c_str(), nullptr};
        ::execv("/bin/bash", const_cast<char* const*>(argv));
        ::_exit(127);
    }
    ::close(pipe_fds[1]);
    ::setpgid(pid, pid); // parent racing arm; EACCES/ESRCH after exec is fine

    // Non-blocking read loop with deadline; poll gives us abort/timeout
    // granularity without busy waiting.
    std::string output;
    bool timed_out = false;
    bool aborted = false;
    struct timespec deadline{};
    ::clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += static_cast<time_t>(request->timeout_seconds);

    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    int status = 0;
    bool reaped = false;
    for (;;) {
        if (should_abort()) {
            aborted = true;
            break;
        }
        struct timespec now{};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        const long remaining_ms =
            (deadline.tv_sec - now.tv_sec) * 1000L + (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remaining_ms <= 0) {
            timed_out = true;
            break;
        }
        struct pollfd pfd{pipe_fds[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, static_cast<int>(std::min<long>(remaining_ms, 100)));
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buffer[4096];
            for (;;) {
                const ssize_t got = ::read(pipe_fds[0], buffer, sizeof(buffer));
                if (got > 0) {
                    output.append(buffer, static_cast<std::size_t>(got));
                    if (output.size() > BuiltinRuntime::kOutputLimitBytes * 4)
                        break; // runaway: rely on the cap below, keep draining
                } else if (got == 0) {
                    break; // EOF
                } else if (errno != EAGAIN && errno != EINTR) {
                    break;
                }
            }
        }
        if (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL)))
            break;
        if (pr > 0 && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
            // Drain then let the reaper below notice the exit.
            char buffer[4096];
            while (::read(pipe_fds[0], buffer, sizeof(buffer)) > 0) {
                output.append(buffer);
            }
        }
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = true;
            // Final drain of whatever the child flushed before exit.
            char buffer[4096];
            while (::read(pipe_fds[0], buffer, sizeof(buffer)) > 0) {
                output.append(buffer);
            }
            break;
        }
    }

    if (!reaped) {
        // Kill the whole group, then reap without blocking forever.
        ::kill(-pid, SIGKILL);
        for (int i = 0; i < 100 && !::waitpid(pid, &status, WNOHANG); ++i) {
            struct timespec nap{0, 20000000}; // 20ms
            ::nanosleep(&nap, nullptr);
        }
    }
    ::close(pipe_fds[0]);

    if (aborted) {
        return ToolResult{"command aborted by user", true};
    }
    if (timed_out) {
        return ToolResult{"command timed out after " + std::to_string(request->timeout_seconds) +
                              "s; process group killed",
                          true};
    }
    auto capped = cap_tool_output(output, BuiltinRuntime::kOutputLimitBytes);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return ToolResult{capped.empty() ? std::string("(no output)") : capped, false};
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::string signal_note;
    if (WIFSIGNALED(status)) {
        code = -WTERMSIG(status);
        signal_note = " (killed by signal)";
    }
    return ToolResult{"exit " + std::to_string(code) + signal_note + "\n" + capped, true};
}

} // namespace mini_agent
