#include "mini_agent/mcp.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mini_agent {
namespace {

constexpr std::size_t kMaxLineBytes = 1024u * 1024u; // 1 MiB per message
constexpr std::size_t kStderrTailBytes = 4096u;

// stdio MCP transport: child process, newline-delimited JSON-RPC
// (spec 2025-03-26 Transports/stdio, re-verified 2026-11-02).
class StdioTransport final : public McpTransport {
  public:
    explicit StdioTransport(McpServerConfig config) : config_(std::move(config)) {
    }
    ~StdioTransport() override {
        close();
    }

    bool start(std::string* error) override {
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
            // Child: wire the pipes, drop our fds, exec argv. Never return.
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
            // Orphan guard (v5): if the mini-agent PROCESS dies without
            // running transport close() -- SIGKILL, crash -- the kernel
            // SIGKILLs this child instead of leaving it reparented. Safe
            // for cross-turn persistence: PDEATHSIG is keyed on the
            // creating thread's PROCESS, not the thread (verified on
            // Linux 6.12: worker-thread exit does NOT fire it, process
            // exit does). getppid()==1 closes the fork-to-prctl race
            // where the parent is already gone.
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() == 1)
                ::_exit(126);
            // Explicit env entries only, plus PATH/HOME baseline (008 5.3):
            // the API key must never reach the child implicitly.
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

    bool alive() const override {
        return child_ > 0 && !exited_;
    }

    bool send(std::string_view message, std::string* error) override {
        if (!alive() || to_child_ < 0) {
            if (error)
                *error = "mcp child process is not running";
            return false;
        }
        if (message.find('\n') != std::string_view::npos) {
            if (error)
                *error = "internal: mcp message contains a newline";
            return false; // framing rule: never embed newlines
        }
        std::string payload(message);
        payload += '\n';
        std::size_t written = 0;
        while (written < payload.size()) {
            const ssize_t n =
                ::write(to_child_, payload.data() + written, payload.size() - written);
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
        if (written < payload.size()) {
            if (error)
                *error = "write to mcp child failed (exit? )";
            return false;
        }
        return true;
    }

    std::optional<std::string> recv(std::chrono::milliseconds timeout,
                                    std::string* error) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            // Try to extract a complete line already buffered.
            if (auto line = take_line())
                return line;
            if (error_out_) {
                if (error)
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
                        if (error)
                            *error = "mcp child stdout closed";
                        exited_ = true;
                        return std::nullopt;
                    }
                }
            }
            reaped_if_dead();
        }
    }

    void close() noexcept override {
        if (child_ <= 0)
            return;
        if (!exited_) {
            if (to_child_ >= 0)
                ::close(to_child_); // EOF: well-behaved servers exit
            to_child_ = -1;
            for (int i = 0; i < 50 && !reaped_if_dead(); ++i) {
                // Drain both pipes while waiting: keeps the stderr
                // diagnostics tail current and prevents the child blocking
                // on a full pipe buffer while we wait for it (review
                // 2026-11-05).
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

    std::string diagnostics_tail() const override {
        return stderr_tail_;
    }

  private:
    // Read whatever stdout has; append to buffer_. False when read hit EOF.
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
                if (n < static_cast<ssize_t>(sizeof(buffer)))
                    continue;
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
            // final drain so buffered lines remain readable
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
    pid_t child_ = -1;
    int to_child_ = -1;
    int from_child_ = -1;
    int from_stderr_ = -1;
    bool exited_ = false;
    bool error_out_ = false;
    std::string buffer_;
    std::string stderr_tail_;
};

} // namespace

std::unique_ptr<McpTransport> make_stdio_transport(const McpServerConfig& config) {
    return std::make_unique<StdioTransport>(config);
}

} // namespace mini_agent
