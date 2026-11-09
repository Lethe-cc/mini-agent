#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "mini_agent/diff.hpp"

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

std::string ToolRegistry::preview(const ToolCall& call) {
    auto* runtime = find(call.name);
    return runtime ? runtime->preview(call) : std::string();
}

void ToolRegistry::disarm(const ToolCall& call) {
    auto* runtime = find(call.name);
    if (runtime)
        runtime->disarm(call);
}

std::string ToolRegistry::result_preview(const ToolCall& call, const ToolResult& result) {
    auto* runtime = find(call.name);
    return runtime ? runtime->result_preview(call, result) : std::string();
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

// Shared head+tail truncation marker (cap_tool_output + BoundedOutputSink
// must emit the identical byte sequence; design 008 5.2).
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
        : limit_(limit_bytes),
          degenerate_(limit_bytes < 64),
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

ToolResult BuiltinRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    if (call.name != "run_bash") {
        return ToolResult{"unknown builtin tool: " + call.name, true};
    }
    std::string arg_error;
    auto request = parse_bash_arguments(call.arguments_json, default_timeout_seconds_, &arg_error);
    if (!request) {
        return ToolResult{arg_error, true};
    }
    const std::vector<const char*> argv = {"/bin/bash", "-lc", request->command.c_str(), nullptr};
    SubprocessSpec spec;
    spec.exec_path = "/bin/bash";
    spec.argv = &argv;
    spec.workspace = workspace_;
    spec.timeout_seconds = request->timeout_seconds;
    return run_subprocess(spec, should_abort);
}

// ---------------------------------------------------------------------------
// v3 workspace boundary + FileRuntime (design 009 section 5.1, ADR 007)
// ---------------------------------------------------------------------------

bool resolve_within_workspace(const std::filesystem::path& root, std::string_view raw,
                              std::filesystem::path* resolved, std::string* error) {
    if (raw.empty()) {
        if (error)
            *error = "empty path";
        return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root_canonical = fs::weakly_canonical(root, ec);
    if (ec) {
        if (error)
            *error = "workspace root unresolvable: " + ec.message();
        return false;
    }
    const fs::path raw_path(raw);
    auto candidate = raw_path.is_absolute() ? raw_path : (root_canonical / raw_path);
    // Lexical normalization first (handles .. against non-existent tails),
    // then canonical where possible: weakly_canonical resolves existing
    // symlinks/components and keeps the not-yet-existing tail.
    candidate = fs::weakly_canonical(candidate, ec);
    if (ec) {
        // Path components with wrong types (e.g. file used as dir).
        if (error)
            *error = "path unresolvable: " + ec.message();
        return false;
    }
    const auto* root_str = root_canonical.native().c_str();
    const auto* cand_str = candidate.native().c_str();
    const auto root_len = root_canonical.native().size();
    (void)root_str;
    if (candidate.native().size() >= root_len &&
        std::char_traits<char>::compare(cand_str, root_str, root_len) == 0) {
        // Boundary: equal to root or a real child (next char is '/').
        if (candidate.native().size() == root_len || candidate.native()[root_len] == '/') {
            // Symlink escape recheck for existing paths: read the final
            // component; a symlink's target must also live inside root.
            if (fs::is_symlink(candidate, ec)) {
                auto target =
                    fs::canonical(candidate.parent_path() / fs::read_symlink(candidate, ec), ec);
                if (!ec) {
                    const auto* t_str = target.native().c_str();
                    const auto t_len = target.native().size();
                    const bool inside =
                        t_len >= root_len &&
                        std::char_traits<char>::compare(t_str, root_str, root_len) == 0 &&
                        (t_len == root_len || target.native()[root_len] == '/');
                    if (!inside) {
                        if (error)
                            *error = "symlink escapes workspace";
                        return false;
                    }
                }
            }
            if (resolved)
                *resolved = std::move(candidate);
            return true;
        }
    }
    if (error)
        *error = "path escapes workspace";
    return false;
}

namespace {

// Shared helper for FileRuntime: string fields with fail-closed errors.
bool get_string(const nlohmann::json& args, const char* key, std::string* out,
                ToolResult* failure) {
    auto it = args.find(key);
    if (it == args.end() || !it->is_string()) {
        *failure = ToolResult{std::string("missing string field '") + key + "'", true};
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

// Applies the edit_file replacement semantics to `old_text`: returns the
// rebuilt text, or an error message. Shared by preview() and call() so the
// diff shown at approval matches what executes (design 009 5.3).
std::string apply_edit(const std::string& old_text, const std::string& old_str,
                       const std::string& new_str, bool replace_all, std::string* error) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = old_text.find(old_str, pos)) != std::string::npos) {
        ++count;
        pos += old_str.size();
    }
    if (count == 0) {
        *error = "old_str not found (0 matches); check exact whitespace/content with read_file";
        return {};
    }
    if (count > 1 && !replace_all) {
        *error = "old_str matches " + std::to_string(count) +
                 " places; add context to make it unique or set replace_all";
        return {};
    }
    std::string remainder = old_text;
    std::string rebuilt;
    while ((pos = remainder.find(old_str)) != std::string::npos) {
        rebuilt.append(remainder, 0, pos);
        rebuilt += new_str;
        remainder.erase(0, pos + old_str.size());
    }
    rebuilt += remainder;
    return rebuilt;
}

} // namespace

FileRuntime::FileRuntime(FileToolsConfig config) : config_(std::move(config)) {
}

std::vector<ToolDefinition> FileRuntime::list() {
    std::vector<ToolDefinition> out;
    auto add = [&out](const char* name, const char* description, const char* schema) {
        ToolDefinition tool;
        tool.name = name;
        tool.description = description;
        tool.input_schema_json = schema;
        tool.source = "builtin";
        out.push_back(tool);
    };
    add("read_file",
        "Read a text file inside the workspace, returned as line-numbered text. "
        "Arguments: {\"path\": \"relative or absolute path\", "
        "\"from_line\": optional first line (1-based), \"to_line\": optional last line}.",
        R"({"type":"object","properties":{"path":{"type":"string"},"from_line":{"type":"integer"},"to_line":{"type":"integer"}},"required":["path"]})");
    add("write_file",
        "Create or fully overwrite a text file inside the workspace (parent dir must "
        "exist). Prefer edit_file for changes to existing files. Arguments: "
        "{\"path\": \"...\", \"content\": \"full new content\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})");
    add("edit_file",
        "Replace an exact string inside a file in the workspace. old_str must match "
        "exactly one place unless replace_all is true (include enough context to be "
        "unique; leading whitespace matters). Arguments: {\"path\": \"...\", "
        "\"old_str\": \"...\", \"new_str\": \"...\", \"replace_all\": optional true}.",
        R"({"type":"object","properties":{"path":{"type":"string"},"old_str":{"type":"string"},"new_str":{"type":"string"},"replace_all":{"type":"boolean"}},"required":["path","old_str","new_str"]})");
    add("list_dir",
        "List a directory inside the workspace (non-recursive). Arguments: "
        "{\"path\": \"optional, default workspace root\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"}}})");
    add("search_text",
        "Search text files in the workspace for a substring or regex, line-based. "
        "Returns \"path:line: text\" rows. Arguments: {\"pattern\": \"...\", "
        "\"is_regex\": optional false, \"path_prefix\": optional subdir/file filter}.",
        R"({"type":"object","properties":{"pattern":{"type":"string"},"is_regex":{"type":"boolean"},"path_prefix":{"type":"string"}},"required":["pattern"]})");
    // v5 (design 011): human-facing file preview. Unlike read_file (which
    // feeds the MODEL line-numbered text), preview_file returns the RAW
    // body so the TUI/CLI can render it for the user (Markdown files are
    // rendered, others shown verbatim). Its result is surfaced through the
    // result_preview() hook, not the model turn.
    add("preview_file",
        "Preview a workspace text file for the human user (the terminal renders it; "
        "Markdown shows formatted). Returns the raw file body, not line numbers. Use "
        "this to SHOW a file to the user; use read_file to inspect content yourself. "
        "Arguments: {\"path\": \"relative or absolute path\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})");
    return out;
}

namespace {

// Reads a whole file with size caps; returns nullopt + ToolResult on any
// failure. Binary probe: NUL byte in the first 8 KiB.
bool read_file_bytes(const std::filesystem::path& path, std::size_t limit, std::string* content,
                     ToolResult* failure) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        *failure = ToolResult{"cannot stat: " + ec.message(), true};
        return false;
    }
    if (size > limit) {
        *failure = ToolResult{"file too large (" + std::to_string(size) + " bytes > " +
                                  std::to_string(limit) +
                                  "); read a line range or use search_text/run_bash head",
                              true};
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *failure = ToolResult{"cannot open file", true};
        return false;
    }
    content->assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return true;
}

std::string relative_to_root(const std::filesystem::path& root,
                             const std::filesystem::path& target) {
    auto root_it = root.begin();
    auto target_it = target.begin();
    while (root_it != root.end() && target_it != target.end() && *root_it == *target_it) {
        ++root_it;
        ++target_it;
    }
    std::filesystem::path rel;
    for (auto it = target_it; it != target.end(); ++it) {
        if (it != target_it)
            rel /= *it;
        else
            rel = *it;
    }
    return rel.empty() ? std::string(".") : rel.string();
}

} // namespace

std::string FileRuntime::preview(const ToolCall& call) {
    if (call.name != "write_file" && call.name != "edit_file")
        return {};
    auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object())
        return {};
    std::string path_raw;
    if (!args.contains("path") || !args["path"].is_string())
        return {};
    path_raw = args["path"].get<std::string>();
    std::filesystem::path resolved;
    std::string bounds_error;
    if (!resolve_within_workspace(config_.workspace, path_raw, &resolved, &bounds_error))
        return {};
    std::error_code ec;
    std::string old_text;
    const bool exists = std::filesystem::is_regular_file(resolved, ec);
    if (exists) {
        std::ifstream stream(resolved, std::ios::binary);
        if (!stream)
            return {};
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        old_text = buffer.str();
    }
    std::string new_text;
    if (call.name == "write_file") {
        if (!args.contains("content") || !args["content"].is_string())
            return {};
        new_text = args["content"].get<std::string>();
    } else { // edit_file
        if (!args.contains("old_str") || !args.contains("new_str") ||
            !args["old_str"].is_string() || !args["new_str"].is_string())
            return {};
        const std::string old_str = args["old_str"].get<std::string>();
        if (old_str.empty())
            return {};
        std::string edit_error;
        new_text = apply_edit(old_text, old_str, args["new_str"].get<std::string>(),
                              args.value("replace_all", false), &edit_error);
        if (!edit_error.empty())
            return "edit_file: " + edit_error; // diff would be invalid; say so at approval
    }
    const auto result = exists ? diff_texts(old_text, new_text) : diff_against_empty(new_text);
    // Arm the race guard with the exact OLD content we previewed.
    armed_previews_[resolved.string()] = old_text;
    const auto label = relative_to_root(config_.workspace, resolved);
    auto text = result.to_unified_text("a/" + label, "b/" + label);
    if (text.empty())
        return "(no content change)";
    return text;
}

void FileRuntime::disarm(const ToolCall& call) {
    if (call.name != "write_file" && call.name != "edit_file")
        return;
    auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object() || !args.contains("path") ||
        !args["path"].is_string())
        return;
    std::filesystem::path resolved;
    std::string error;
    if (!resolve_within_workspace(config_.workspace, args["path"].get<std::string>(), &resolved,
                                  &error))
        return;
    armed_previews_.erase(resolved.string());
}

// v5 (design 011): post-execution transcript preview. write_file/edit_file
// -> the APPLIED unified diff staged by call(); preview_file -> the raw
// body staged for the UI viewer. Consumed once (staged state cleared) so a
// later unrelated tool call cannot resend stale content.
std::string FileRuntime::result_preview(const ToolCall& call, const ToolResult& result) {
    if (result.is_error)
        return {};
    if (call.name == "write_file" || call.name == "edit_file") {
        std::string diff;
        diff.swap(last_applied_diff_);
        last_applied_path_.clear();
        if (diff.empty())
            return {};
        return diff;
    }
    if (call.name == "preview_file") {
        std::string body;
        body.swap(last_preview_body_);
        last_preview_path_.clear();
        return body;
    }
    return {};
}

ToolResult FileRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    namespace fs = std::filesystem;
    ToolResult bad{"arguments must be a JSON object", true};
    auto parsed = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return bad;
    auto resolve = [&](const std::string& raw, fs::path* out) -> std::optional<ToolResult> {
        std::string error;
        if (!resolve_within_workspace(config_.workspace, raw, out, &error))
            return ToolResult{raw + ": " + error, true};
        return std::nullopt;
    };
    auto abort_check = [&]() -> std::optional<ToolResult> {
        if (should_abort())
            return ToolResult{"aborted by user", true};
        return std::nullopt;
    };

    if (call.name == "read_file") {
        std::string path_raw;
        if (!get_string(parsed, "path", &path_raw, &bad))
            return bad;
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec))
            return ToolResult{path_raw + ": not a regular file", true};
        std::string content;
        if (!read_file_bytes(resolved, config_.read_limit_bytes, &content, &bad))
            return bad;
        if (content.find('\0') != std::string::npos)
            return ToolResult{path_raw + ": binary file (NUL found); use run_bash", true};
        // Line-range slice with cat -n numbering.
        const auto lines = diff_split_lines(content);
        std::size_t from = 1;
        std::size_t to = lines.size();
        if (parsed.contains("from_line") && parsed["from_line"].is_number_integer())
            from = static_cast<std::size_t>(
                std::max<std::int64_t>(1, parsed["from_line"].get<int64_t>()));
        if (parsed.contains("to_line") && parsed["to_line"].is_number_integer())
            to = static_cast<std::size_t>(
                std::min(std::max<std::int64_t>(0, parsed["to_line"].get<int64_t>()),
                         static_cast<std::int64_t>(lines.size())));
        if (lines.empty())
            return ToolResult{"(empty file)", false};
        if (from > to || from > lines.size())
            return ToolResult{"empty line range", false};
        std::ostringstream out;
        for (std::size_t i = from; i <= to; ++i) {
            if (auto early = abort_check())
                return *early;
            out << i << '\t' << lines[i - 1] << '\n';
        }
        std::string shown = out.str();
        if (shown.size() > config_.read_limit_bytes)
            shown = cap_tool_output(shown, config_.read_limit_bytes);
        return ToolResult{shown + "\n(" + path_raw + ": " + std::to_string(to - from + 1) + " of " +
                              std::to_string(lines.size()) + " lines)",
                          false};
    }

    if (call.name == "preview_file") {
        // v5 (design 011): raw body for the human viewer (the UI renders
        // it; the model gets only a short confirmation back). Same
        // boundary, size cap, and binary posture as read_file.
        std::string path_raw;
        if (!get_string(parsed, "path", &path_raw, &bad))
            return bad;
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec))
            return ToolResult{path_raw + ": not a regular file", true};
        std::string content;
        if (!read_file_bytes(resolved, config_.read_limit_bytes, &content, &bad))
            return bad;
        if (content.find('\0') != std::string::npos)
            return ToolResult{path_raw + ": binary file (NUL found); use run_bash", true};
        if (content.empty())
            return ToolResult{"(empty file)", false};
        // The body is carried to the UI via result_preview(); the model
        // only needs to know it was shown (keeps tokens out of the chat).
        const std::size_t body_bytes = content.size();
        last_preview_path_ = path_raw;
        last_preview_body_ = std::move(content);
        return ToolResult{"previewed " + path_raw + " (" + std::to_string(body_bytes) +
                              " bytes) for the user",
                          false};
    }

    if (call.name == "write_file" || call.name == "edit_file") {
        std::string path_raw;
        if (!get_string(parsed, "path", &path_raw, &bad))
            return bad;
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        // v5 (design 011): the pre-write body, captured for the applied
        // diff staged after a successful rename. Empty/absent => the file
        // is new and the diff runs against empty.
        std::string pre_image;
        bool has_pre_image = false;
        // Race guard (design 009 5.3): if a preview armed the expected old
        // content, refuse when the file changed underneath us since. Runs
        // BEFORE any content work so the refusal is about the race, not a
        // spurious "not found" from the mutated file.
        auto armed = armed_previews_.find(resolved.string());
        if (armed != armed_previews_.end()) {
            std::string current;
            std::ifstream stream(resolved, std::ios::binary);
            bool readable = static_cast<bool>(stream);
            if (readable) {
                std::ostringstream buffer;
                buffer << stream.rdbuf();
                current = buffer.str();
            }
            const bool gone = !readable && !fs::exists(resolved, ec);
            // Unreadable-but-existing: treat as changed (cannot verify).
            // Deleted after a non-empty preview: also changed (the approved
            // diff's base vanished). Deleted after an empty preview (new
            // file): still the creation the user approved.
            const bool mismatch =
                readable ? (current != armed->second) : (gone ? !armed->second.empty() : true);
            if (mismatch) {
                armed_previews_.erase(armed);
                return ToolResult{
                    path_raw + ": changed since the approved preview; re-read and retry", true};
            }
            // The file is byte-identical to the approved preview: that
            // snapshot IS the pre-image (and it also covers a new file:
            // armed empty + not readable => not the mismatch branch).
            pre_image = armed->second;
            has_pre_image = true;
            armed_previews_.erase(armed);
        }
        std::string new_text;
        if (call.name == "write_file") {
            if (!get_string(parsed, "content", &new_text, &bad))
                return bad;
        } else {
            std::string old_str;
            std::string new_str;
            if (!get_string(parsed, "old_str", &old_str, &bad))
                return bad;
            if (!get_string(parsed, "new_str", &new_str, &bad))
                return bad;
            if (old_str.empty())
                return ToolResult{"edit_file: 'old_str' must be non-empty", true};
            std::string& old_text = pre_image; // doubles as the pre-image
            has_pre_image = true;              // for the applied diff below
            if (!read_file_bytes(resolved, config_.write_limit_bytes, &old_text, &bad))
                return bad;
            std::string edit_error;
            new_text = apply_edit(old_text, old_str, new_str, parsed.value("replace_all", false),
                                  &edit_error);
            if (!edit_error.empty())
                return ToolResult{"edit_file: " + edit_error + " in " + path_raw, true};
        }
        if (new_text.size() > config_.write_limit_bytes)
            return ToolResult{"content too large for the write cap", true};
        if (auto early = abort_check())
            return *early;
        if (!fs::exists(resolved.parent_path(), ec))
            return ToolResult{path_raw + ": parent directory does not exist", true};
        const bool existed = fs::exists(resolved, ec);
        const std::string tmp = resolved.string() + ".mini-agent.tmp";
        {
            // Mode applies only on create; existing files keep theirs via
            // O_TRUNC. New files: 0644 & ~umask (same posture as any editor).
            const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
                return ToolResult{path_raw + ": open failed: " + std::strerror(errno), true};
            const ssize_t written = ::write(fd, new_text.data(), new_text.size());
            const bool ok = written == static_cast<ssize_t>(new_text.size());
            if (ok)
                ::fsync(fd);
            ::close(fd);
            if (!ok) {
                ::unlink(tmp.c_str());
                return ToolResult{path_raw + ": short write", true};
            }
        }
        // Preserve mode for existing files; new files get 0644 & ~umask via
        // open above. rename is atomic within the directory.
        if (::rename(tmp.c_str(), resolved.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return ToolResult{path_raw + ": rename failed: " + std::strerror(errno), true};
        }
        // v5 (design 011): stage the APPLIED diff for result_preview().
        // Pre-image sources, in order: the race-guard snapshot (approval
        // flow), the re-read body (edit_file), else the file as it stands
        // right before this write (write_file without a preview). On any
        // read failure the diff falls back to "new file against empty".
        if (!has_pre_image) {
            if (existed) {
                std::ifstream before(resolved, std::ios::binary);
                if (before) {
                    std::ostringstream buffer;
                    buffer << before.rdbuf();
                    pre_image = buffer.str();
                }
            } else {
                has_pre_image = true; // new file: empty pre-image is final
            }
        }
        {
            const auto result =
                pre_image.empty() ? diff_against_empty(new_text) : diff_texts(pre_image, new_text);
            const auto label = relative_to_root(config_.workspace, resolved);
            last_applied_path_ = path_raw;
            last_applied_diff_ =
                result.changed ? result.to_unified_text("a/" + label, "b/" + label) : std::string();
        }
        return ToolResult{std::string(existed ? "wrote " : "created ") + path_raw + " (" +
                              std::to_string(new_text.size()) + " bytes)",
                          false};
    }

    if (call.name == "list_dir") {
        std::string path_raw = ".";
        if (parsed.contains("path") && parsed["path"].is_string())
            path_raw = parsed["path"].get<std::string>();
        else if (parsed.contains("path"))
            return ToolResult{"'path' must be a string", true};
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        if (!fs::is_directory(resolved, ec))
            return ToolResult{path_raw + ": not a directory", true};
        std::vector<std::string> entries;
        for (fs::directory_iterator it(resolved, ec), end; it != end; it.increment(ec)) {
            if (ec)
                break;
            if (auto early = abort_check())
                return *early;
            const auto name = it->path().filename().string();
            std::string suffix;
            if (it->is_directory(ec))
                suffix = "/";
            else if (it->is_symlink(ec))
                suffix = "@";
            entries.push_back(name + suffix);
            if (entries.size() >= config_.list_max_entries)
                break;
        }
        std::sort(entries.begin(), entries.end());
        std::ostringstream out;
        for (const auto& entry : entries)
            out << entry << '\n';
        if (entries.size() >= config_.list_max_entries)
            out << "(listing capped at " << config_.list_max_entries << " entries)\n";
        return ToolResult{out.str().empty() ? std::string("(empty directory)") : out.str(), false};
    }

    if (call.name == "search_text") {
        std::string pattern;
        if (!get_string(parsed, "pattern", &pattern, &bad))
            return bad;
        if (pattern.empty())
            return ToolResult{"'pattern' must be non-empty", true};
        const bool is_regex = parsed.value("is_regex", false);
        std::string prefix_raw = ".";
        if (parsed.contains("path_prefix") && parsed["path_prefix"].is_string())
            prefix_raw = parsed["path_prefix"].get<std::string>();
        fs::path resolved;
        if (auto early = resolve(prefix_raw, &resolved))
            return *early;
        std::ostringstream out;
        std::size_t matches = 0;
        bool cap_hit = false;
        std::regex re;
        if (is_regex) {
            try {
                re.assign(pattern, std::regex::optimize);
            } catch (const std::regex_error& error) {
                return ToolResult{"invalid regex: " + std::string(error.what()), true};
            }
        }
        std::error_code ec;
        if (fs::is_regular_file(resolved, ec)) {
            // Single-file mode: search one file directly.
            std::string content;
            if (!read_file_bytes(resolved, config_.write_limit_bytes, &content, &bad))
                return bad;
            const auto lines = diff_split_lines(content);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const bool hit = is_regex ? std::regex_search(std::string(lines[i]), re)
                                          : lines[i].find(pattern) != std::string::npos;
                if (!hit)
                    continue;
                out << prefix_raw << ':' << (i + 1) << ": " << lines[i] << '\n';
                if (++matches >= config_.search_max_matches) {
                    cap_hit = true;
                    break;
                }
            }
        } else {
            std::error_code iter_ec;
            fs::recursive_directory_iterator it(
                resolved, fs::directory_options::skip_permission_denied, iter_ec);
            if (iter_ec)
                return ToolResult{prefix_raw + ": cannot open directory: " + iter_ec.message(),
                                  true};
            const fs::recursive_directory_iterator end;
            for (; it != end; it.increment(iter_ec)) {
                if (iter_ec)
                    break;
                if (auto early = abort_check())
                    return *early;
                if (it.depth() > 0 && it->path().filename() == ".git") {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code type_ec;
                if (!it->is_regular_file(type_ec) || type_ec)
                    continue;
                const auto rel = relative_to_root(config_.workspace, it->path());
                std::error_code size_ec;
                const auto size = fs::file_size(it->path(), size_ec);
                if (size_ec || size > config_.write_limit_bytes)
                    continue; // skip unreadable/huge without failing the search
                std::string content;
                ToolResult read_failure{"", false};
                if (!read_file_bytes(it->path(), size, &content, &read_failure))
                    continue;
                if (content.find('\0') != std::string::npos)
                    continue; // binary
                const auto lines = diff_split_lines(content);
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    const bool hit = is_regex ? std::regex_search(std::string(lines[i]), re)
                                              : lines[i].find(pattern) != std::string::npos;
                    if (!hit)
                        continue;
                    out << rel << ':' << (i + 1) << ": " << lines[i] << '\n';
                    if (++matches >= config_.search_max_matches) {
                        cap_hit = true;
                        break;
                    }
                }
                if (cap_hit)
                    break;
            }
        }
        if (cap_hit)
            out << "(match cap hit at " << config_.search_max_matches << ")\n";
        auto text = out.str();
        if (text.empty())
            return ToolResult{"(no matches)", false};
        return ToolResult{cap_tool_output(text, BuiltinRuntime::kOutputLimitBytes), false};
    }

    return ToolResult{"unknown file tool: " + call.name, true};
}

// ---------------------------------------------------------------------------
// GitRuntime (design 009 5.2, ADR 008)
// ---------------------------------------------------------------------------

GitRuntime::GitRuntime(std::filesystem::path workspace, std::size_t timeout_seconds)
    : workspace_(std::move(workspace)), timeout_seconds_(timeout_seconds) {
}

void GitRuntime::ensure_probed() const {
    if (probed_)
        return;
    probed_ = true;
    if (workspace_.empty()) {
        alive_ = false;
        return;
    }
    // Two-in-one probe: git runs, and the workspace is inside a work tree.
    // --no-optional-locks keeps the probe from touching repo state.
    const std::string ws = workspace_.string();
    const std::vector<const char*> argv = {"git",
                                           "-C",
                                           ws.c_str(),
                                           "--no-pager",
                                           "--no-optional-locks",
                                           "rev-parse",
                                           "--is-inside-work-tree",
                                           nullptr};
    SubprocessSpec spec;
    spec.exec_path = "git";
    spec.argv = &argv;
    spec.timeout_seconds = 5;
    auto result = run_subprocess(spec, [] { return false; });
    alive_ = !result.is_error && result.text.find("true") != std::string::npos;
}

bool GitRuntime::alive() const {
    ensure_probed();
    return alive_;
}

std::vector<ToolDefinition> GitRuntime::list() {
    std::vector<ToolDefinition> out;
    if (!alive())
        return out; // dead source: no tools advertised (008 5.5 posture)
    auto add = [&out](const char* name, const char* description, const char* schema) {
        ToolDefinition tool;
        tool.name = name;
        tool.description = description;
        tool.input_schema_json = schema;
        tool.source = "builtin-git";
        out.push_back(tool);
    };
    add("git_status",
        "Show workspace repository status (git status --porcelain=v1 -b plus a readable "
        "summary). Read-only. Arguments: {}.",
        R"({"type":"object","properties":{}})");
    add("git_diff",
        "Show the working-tree diff. Arguments: {\"staged\": optional true to diff the "
        "index, \"path\": optional file/dir filter (workspace-relative)}.",
        R"({"type":"object","properties":{"staged":{"type":"boolean"},"path":{"type":"string"}}})");
    add("git_log",
        "Show recent commits as one line each. Arguments: {\"limit\": optional 1-100, "
        "default 20}.",
        R"({"type":"object","properties":{"limit":{"type":"integer"}}})");
    add("git_add",
        "Stage workspace paths (git add -- <path>); paths must be inside the workspace. "
        "Arguments: {\"path\": \"file or directory path\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})");
    add("git_commit",
        "Create a LOCAL commit from staged changes (git commit; never pushes). The "
        "approval view shows the file list and diff before anything happens. Arguments: "
        "{\"message\": \"subject line\", \"body\": optional paragraph text}.",
        R"({"type":"object","properties":{"message":{"type":"string"},"body":{"type":"string"}},"required":["message"]})");
    return out;
}

ToolResult GitRuntime::run_git(const std::vector<std::string>& args,
                               const ShouldAbort& should_abort) const {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& part : args)
        argv.push_back(part.c_str());
    argv.push_back(nullptr);
    SubprocessSpec spec;
    spec.exec_path = "git";
    spec.argv = &argv;
    spec.workspace = workspace_;
    spec.timeout_seconds = timeout_seconds_;
    return run_subprocess(spec, should_abort);
}

bool parse_porcelain_line(std::string_view line, std::string* status, std::string* path) {
    // Porcelain v1: "XY path" (X=index status, Y=worktree status). Rename
    // entries carry " -> " with NUL quoting disabled; we pass -z nowhere,
    // so quote-escapes may appear -- acceptable for display/preview.
    if (line.size() < 4 || line[0] == '#' || line[0] == '!')
        return false;
    const bool space = line[2] == ' ';
    if (!space)
        return false;
    *status = std::string(line.substr(0, 2));
    *path = std::string(line.substr(3));
    return !path->empty();
}

namespace {

// Base helper: common git flags for every invocation.
std::vector<std::string> git_base(const std::filesystem::path& workspace) {
    return {"git", "-C", workspace.string(), "--no-pager", "--no-optional-locks"};
}

} // namespace

std::string GitRuntime::preview(const ToolCall& call) {
    if (call.name != "git_commit" || !alive())
        return {};
    // Read-only preview: what WOULD be committed. Unstaged changes are
    // listed too so the human sees the gap (git commit takes staged only).
    std::string out;
    auto status_args = git_base(workspace_);
    status_args.insert(status_args.end(), {"status", "--porcelain=v1"});
    const auto status = run_git(status_args, [] { return false; });
    if (status.is_error)
        return {};
    std::size_t staged_count = 0;
    std::size_t unstaged_count = 0;
    std::vector<std::string> staged_files;
    std::istringstream stream(status.text);
    std::string line;
    while (std::getline(stream, line)) {
        std::string code;
        std::string file;
        if (!parse_porcelain_line(line, &code, &file))
            continue;
        const bool staged_change = code[0] != ' ' && code[0] != '?';
        const bool unstaged_change = code[1] != ' ';
        if (staged_change) {
            ++staged_count;
            staged_files.push_back(file);
        }
        if (unstaged_change)
            ++unstaged_count;
    }
    if (staged_count == 0)
        return "(nothing staged -- git_commit would fail unless git add runs first; " +
               std::to_string(unstaged_count) + " unstaged change(s) exist)";
    out += "commit will include " + std::to_string(staged_count) + " staged file(s)";
    if (unstaged_count)
        out += " (" + std::to_string(unstaged_count) + " other changes stay unstaged)";
    out += ":\n";
    for (std::size_t i = 0; i < staged_files.size() && i < 30; ++i)
        out += "  " + staged_files[i] + "\n";
    if (staged_files.size() > 30)
        out += "  ... +" + std::to_string(staged_files.size() - 30) + " more\n";
    // Staged diff stat for the eyes (budget-limited, 009 5.3 posture).
    auto diff_args = git_base(workspace_);
    diff_args.insert(diff_args.end(), {"diff", "--cached", "--stat", "--no-color"});
    const auto diff = run_git(diff_args, [] { return false; });
    if (!diff.is_error && diff.text != "(no output)") {
        out += diff.text;
    }
    return out;
}

ToolResult GitRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    if (!alive())
        return ToolResult{"git unavailable (binary missing or not a repository)", true};
    auto parsed = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return ToolResult{"arguments must be a JSON object", true};
    auto abort_check = [&]() -> std::optional<ToolResult> {
        if (should_abort())
            return ToolResult{"aborted by user", true};
        return std::nullopt;
    };

    if (call.name == "git_status") {
        auto args = git_base(workspace_);
        args.insert(args.end(), {"status", "--porcelain=v1", "--branch"});
        return run_git(args, should_abort);
    }
    if (call.name == "git_diff") {
        auto args = git_base(workspace_);
        args.push_back("diff");
        args.push_back("--no-color");
        if (parsed.value("staged", false))
            args.push_back("--cached");
        if (parsed.contains("path")) {
            if (!parsed["path"].is_string())
                return ToolResult{"'path' must be a string", true};
            std::filesystem::path resolved;
            std::string error;
            if (!resolve_within_workspace(workspace_, parsed["path"].get<std::string>(), &resolved,
                                          &error))
                return ToolResult{"diff path: " + error, true};
            args.push_back("--");
            args.push_back(parsed["path"].get<std::string>());
        }
        return run_git(args, should_abort);
    }
    if (call.name == "git_log") {
        int64_t limit = 20;
        if (parsed.contains("limit")) {
            if (!parsed["limit"].is_number_integer() || parsed["limit"].get<int64_t>() < 1 ||
                parsed["limit"].get<int64_t>() > 100)
                return ToolResult{"'limit' must be an integer in 1..100", true};
            limit = parsed["limit"].get<int64_t>();
        }
        auto args = git_base(workspace_);
        args.insert(args.end(), {"log", "--oneline", "--decorate=no", "-" + std::to_string(limit)});
        return run_git(args, should_abort);
    }
    if (call.name == "git_add") {
        if (!parsed.contains("path") || !parsed["path"].is_string())
            return ToolResult{"git_add requires string 'path'", true};
        const std::string path_raw = parsed["path"].get<std::string>();
        std::filesystem::path resolved;
        std::string error;
        if (!resolve_within_workspace(workspace_, path_raw, &resolved, &error))
            return ToolResult{"add path: " + error, true};
        if (auto early = abort_check())
            return *early;
        auto args = git_base(workspace_);
        args.insert(args.end(), {"add", "--", path_raw});
        auto result = run_git(args, should_abort);
        if (!result.is_error)
            result.text = "staged: " + path_raw;
        return result;
    }
    if (call.name == "git_commit") {
        if (!parsed.contains("message") || !parsed["message"].is_string() ||
            parsed["message"].get<std::string>().empty())
            return ToolResult{"git_commit requires non-empty string 'message'", true};
        std::string message = parsed["message"].get<std::string>();
        if (message.find('\n') != std::string::npos)
            return ToolResult{"'message' must be a single subject line; put paragraphs in 'body'",
                              true};
        std::string body;
        if (parsed.contains("body")) {
            if (!parsed["body"].is_string())
                return ToolResult{"'body' must be a string", true};
            body = parsed["body"].get<std::string>();
        }
        if (auto early = abort_check())
            return *early;
        // Fixed argv: subject via -m, optional body via second -m. Model
        // text NEVER forms a command, only a dedicated argument slot.
        auto args = git_base(workspace_);
        args.insert(args.end(), {"commit", "-m", message});
        if (!body.empty()) {
            args.push_back("-m");
            args.push_back(body);
        }
        auto result = run_git(args, should_abort);
        if (!result.is_error) {
            // Compact the verbose default output to the summary line.
            auto log_args = git_base(workspace_);
            log_args.insert(log_args.end(), {"log", "-1", "--oneline"});
            auto head = run_git(log_args, should_abort);
            result.text = head.is_error ? "committed" : ("committed: " + head.text);
        }
        return result;
    }
    if (call.name == "git_push" || call.name == "git_pull" || call.name == "git_fetch" ||
        call.name == "git_reset" || call.name == "git_rebase" || call.name == "git_checkout") {
        return ToolResult{call.name +
                              ": not provided -- remote and history-rewriting operations are "
                              "human-only (ADR 008); ask the user to run them",
                          true};
    }
    return ToolResult{"unknown git tool: " + call.name, true};
}

} // namespace mini_agent
