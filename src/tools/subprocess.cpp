#include "mini_agent/tools.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace mini_agent {
constexpr std::string_view kOutputTruncationMarker = "\n[... output truncated ...]\n";

std::string cap_tool_output(std::string_view text, std::size_t limit_bytes) {
    if (text.size() <= limit_bytes || limit_bytes < 64)
        return std::string(text);
    const std::string marker(kOutputTruncationMarker);
    const std::size_t half = (limit_bytes - marker.size()) / 2;
    std::string out;
    out.append(text.substr(0, half));
    out += marker;
    out.append(text.substr(text.size() - half));
    return out;
}

namespace {
// v4.17: bounded head+tail sink for run_subprocess (design 008 5.2 cap
// semantics, byte-identical to cap_tool_output on the same stream). The
// old loop could buffer 4x the cap of a runaway producer before trimming,
// and its "stop appending, keep draining" branch silently dropped the
// true tail -- exit-cascade errors live there. The sink holds at most
// ~1.5x the cap (a verbatim prefix + a last-`half` ring) and the final
// tail is always the real stream end.

class BoundedOutputSink {
  public:
    // `limit_bytes` must be the same cap passed to cap_tool_output; below
    // 64 the cap function returns text verbatim, so the sink mirrors that
    // by buffering everything (degenerate mode) instead of ringing.
    explicit BoundedOutputSink(std::size_t limit_bytes)
        : limit_(limit_bytes), degenerate_(limit_bytes < 64),
          half_(degenerate_ ? limit_bytes : (limit_bytes - kOutputTruncationMarker.size()) / 2),
          ring_(degenerate_ ? 1 : half_, '\0') {
        prefix_.reserve(limit_bytes + 1024);
    }
    void append(std::string_view chunk) {
        total_ += chunk.size();
        if (degenerate_) {
            prefix_.append(chunk); // tiny limit: cap_tool_output is verbatim anyway
            return;
        }
        if (prefix_.size() < limit_) {
            const std::size_t take = std::min(chunk.size(), limit_ - prefix_.size());
            prefix_.append(chunk.data(), take);
            chunk.remove_prefix(take);
        }
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            ring_[write_] = chunk[i];
            write_ = (write_ + 1) % ring_.size();
        }
    }
    std::string finish() const {
        if (degenerate_ || total_ <= limit_)
            return prefix_; // fits: verbatim, same as cap_tool_output
        std::string out = prefix_.substr(0, half_);
        out += kOutputTruncationMarker;
        const std::size_t ring_cap = ring_.size();
        const std::size_t ringed = total_ - limit_ > ring_cap ? ring_cap : total_ - limit_;
        if (total_ - limit_ >= ring_cap) {
            // Ring wrapped: last half bytes are the rotation from write_.
            out.append(ring_, write_, ring_cap - write_);
            out.append(ring_, 0, write_);
        } else {
            // Ring underfilled: the tail straddles prefix end and ring start.
            const std::size_t tail_start = total_ - half_;
            if (tail_start < limit_)
                out.append(prefix_, tail_start, limit_ - tail_start);
            out.append(ring_, 0, ringed);
        }
        return out;
    }

  private:
    std::size_t limit_ = 0;
    bool degenerate_ = false;
    std::size_t half_ = 0;
    std::string prefix_;
    std::string ring_; // circular last-half buffer (may be size 1 in degenerate mode)
    std::size_t write_ = 0;
    std::size_t total_ = 0;
};
} // namespace

ToolResult run_subprocess(const SubprocessSpec& spec, const ShouldAbort& should_abort) {
    const std::size_t limit =
        spec.output_limit_bytes ? spec.output_limit_bytes : BuiltinRuntime::kOutputLimitBytes;
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
        // v3: run inside the workspace root when configured (009 5.4).
        if (!spec.workspace.empty()) {
            if (::chdir(spec.workspace.c_str()) != 0)
                ::_exit(126); // boundary requested but unreachable
        }
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        // Ensure fd 0 is valid for the child without clobbering a live stdin.
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
        // v3: `exec_path` may be a bare program name (git) resolved via
        // PATH or an absolute path (/bin/bash); execvp covers both.
        ::execvp(spec.exec_path, const_cast<char* const*>(spec.argv->data()));
        ::_exit(127);
    }
    ::close(pipe_fds[1]);
    ::setpgid(pid, pid); // parent racing arm; EACCES/ESRCH after exec is fine

    // Non-blocking read loop with deadline; poll gives us abort/timeout
    // granularity without busy waiting.
    BoundedOutputSink output(limit);
    bool timed_out = false;
    bool aborted = false;
    struct timespec deadline{};
    ::clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += static_cast<time_t>(spec.timeout_seconds);

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
                    // v4.17: the sink is bounded -- a runaway producer can
                    // no longer balloon memory or silently drop the tail,
                    // so this loop always drains to EAGAIN/EOF.
                    output.append(std::string_view(buffer, static_cast<std::size_t>(got)));
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
            ssize_t got = 0;
            while ((got = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
                output.append(std::string_view(buffer, static_cast<std::size_t>(got)));
            }
        }
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = true;
            // Final drain of whatever the child flushed before exit.
            char buffer[4096];
            ssize_t got = 0;
            while ((got = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
                output.append(std::string_view(buffer, static_cast<std::size_t>(got)));
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
        return ToolResult{"command timed out after " + std::to_string(spec.timeout_seconds) +
                              "s; process group killed",
                          true};
    }
    auto capped = output.finish(); // same head+tail bytes as cap_tool_output
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
