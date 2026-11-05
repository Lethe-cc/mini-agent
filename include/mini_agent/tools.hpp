#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/provider.hpp"

namespace mini_agent {

// v2 tool layer (design 008 section 5.2). One registry aggregates tool
// sources (builtin runtime + MCP servers) behind one neutral shape; the
// agent loop is source-agnostic.

struct ToolDefinition {
    std::string name;              // unique across the registry
    std::string description;       // model-visible: UNTRUSTED content
    std::string input_schema_json; // JSON Schema, passed through opaquely
    std::string source;            // "builtin" or the MCP server name
};

struct ToolResult {
    std::string text; // model-visible: UNTRUSTED, length-capped by callers
    bool is_error = false;
    long long elapsed_ms = 0; // measured by the loop around execute_one (UI)
};

// One tool source. Implementations must not throw for ordinary tool
// failures -- fold everything into ToolResult{is_error=true} so the model
// can self-correct (design 008 section 5.2). Throwing signals a runtime
// fault and aborts the turn like a ProviderError.
class ToolRuntime {
  public:
    virtual ~ToolRuntime() = default;
    virtual std::vector<ToolDefinition> list() = 0;
    virtual ToolResult call(const ToolCall& call, const ShouldAbort& should_abort) = 0;
    // False when the source is known dead (e.g. MCP server down): its
    // tools disappear from the next listing without crashing the loop.
    virtual bool alive() const {
        return true;
    }
    // v3 (design 009 section 5.3): optional human-readable preview shown
    // in the approval block instead of the bare argument summary (file
    // writes and commits show a unified diff / staged summary). Read-only
    // by contract: computing the preview also arms the preview-vs-execute
    // race guard in stateful runtimes. Empty string = no preview.
    virtual std::string preview(const ToolCall& call) {
        (void)call;
        return {};
    }
    // v3: called when an approval was DENIED after preview() armed state.
    // Stateful runtimes should drop any preview/guard association so a
    // later identical call is not refused by a stale race guard. No-op by
    // default.
    virtual void disarm(const ToolCall& call) {
        (void)call;
    }
};

class ToolRegistry {
  public:
    // Ownership by insertion order; duplicate tool names are rejected at
    // add time so listings stay deterministic (design 008 section 5.2).
    void add(std::unique_ptr<ToolRuntime> runtime);
    bool empty() const noexcept {
        return runtimes_.empty();
    }

    std::vector<ToolDefinition> list();
    // Runtime owning `name`, or nullptr.
    ToolRuntime* find(const std::string& name);

    // v3: delegates to the owning runtime's preview() (empty when the
    // tool has none or the name is unknown). Callers must honor the same
    // thread contract as find()/list() -- the loop/approver thread only.
    std::string preview(const ToolCall& call);
    // v3: denial cleanup hook, same thread contract as preview().
    void disarm(const ToolCall& call);

    // OpenAI wire array: [{type:"function", function:{name, description,
    // parameters}}]. "[]" when empty -- byte-identical to a no-tools v1
    // request (design 008 section 7 regression hook).
    std::string to_openai_tools_json();

  private:
    std::vector<std::unique_ptr<ToolRuntime>> runtimes_;
    std::vector<std::string> names_; // claim list for duplicate detection
};

// Approval outcome for one invocation (design 008 section 5.4).
enum class Approval { Allow, Deny, AllowSession };

// The loop asks before every execution when an approver is installed.
// Returning Deny produces an is_error Tool message so the model learns
// the human refused (no silent drops).
using ToolApprover = std::function<Approval(const ToolCall&)>;

// Builtin runtime backed by POSIX fork/exec of /bin/bash (design 008
// section 5.2): `run_bash` with argv fixed as {bash, -lc, command}.
// Output = stdout+stderr merged, capped; timeout kills the whole process
// group. No sandbox: runs with the user's own permissions -- approval is
// the control (ADR 006).
class BuiltinRuntime final : public ToolRuntime {
  public:
    static constexpr std::size_t kOutputLimitBytes = 32u * 1024u;

    // v3: workspace root for run_bash child cwd (design 009 section 5.4);
    // empty = inherit the process cwd (v2 behavior).
    explicit BuiltinRuntime(std::size_t default_timeout_seconds = 30,
                            std::filesystem::path workspace = {})
        : default_timeout_seconds_(default_timeout_seconds), workspace_(std::move(workspace)) {
    }

    std::vector<ToolDefinition> list() override;
    ToolResult call(const ToolCall& call, const ShouldAbort& should_abort) override;

  private:
    std::size_t default_timeout_seconds_;
    std::filesystem::path workspace_;
};

// Parses run_bash arguments ({command, timeout?}); nullopt + *error on
// malformed input (fail-closed). Exposed for tests.
struct BashRequest {
    std::string command;
    std::size_t timeout_seconds = 0; // 0 = default
};
std::optional<BashRequest> parse_bash_arguments(std::string_view arguments_json,
                                                std::size_t default_timeout_seconds,
                                                std::string* error);

// v3 shared POSIX subprocess runner (design 009 section 5.2): fork/exec
// with merged stdout+stderr capture, process-group timeout kill, abort
// hook, and output cap. `argv` must be null-terminated and stay alive for
// the call; fixed-argv execution only -- no shell involved (run_bash is
// the explicit exception, passing itself as the program). Child chdirs to
// `workspace` when non-empty; a failed chdir exits 126.
struct SubprocessSpec {
    const char* exec_path = nullptr;
    const std::vector<const char*>* argv = nullptr;
    std::filesystem::path workspace; // empty = inherit process cwd
    std::size_t timeout_seconds = 30;
    std::size_t output_limit_bytes = 0; // 0 = 32 KiB default
};
ToolResult run_subprocess(const SubprocessSpec& spec, const ShouldAbort& should_abort);

// Truncates tool output keeping head and tail halves with a marker
// (design 008 section 5.2). Pure and tested.
std::string cap_tool_output(std::string_view text, std::size_t limit_bytes);

// ---------------------------------------------------------------------------
// v3 FileRuntime (design 009 section 5.1): five first-class file tools
// inside a workspace boundary. Soft boundary (ADR 007): path resolution
// via weakly_canonical + symlink-target recheck; escapes return is_error
// Tool messages the model can self-correct from. No delete tool exists.
// ---------------------------------------------------------------------------

// Resolves `raw` against `root` (relative or absolute), normalizing `.`
// and `..` lexically and requiring the canonical result to stay inside
// the canonical root. On success *resolved carries the canonical path.
// Fail-closed: false + *error on escape attempts or empty roots. Symlink
// targets are re-checked by the caller after resolution (readlink walk).
bool resolve_within_workspace(const std::filesystem::path& root, std::string_view raw,
                              std::filesystem::path* resolved, std::string* error);

struct FileToolsConfig {
    std::filesystem::path workspace; // canonical at construction
    std::size_t read_limit_bytes = 64u * 1024u;
    std::size_t write_limit_bytes = 4u * 1024u * 1024u;
    std::size_t search_max_matches = 200;
    std::size_t list_max_entries = 512;
};

class FileRuntime final : public ToolRuntime {
  public:
    explicit FileRuntime(FileToolsConfig config);

    std::vector<ToolDefinition> list() override;
    ToolResult call(const ToolCall& call, const ShouldAbort& should_abort) override;
    // v3 (design 009 5.3): unified-diff preview for write_file/edit_file
    // (arms the race guard: call() re-reads the target and refuses if it
    // changed since).
    std::string preview(const ToolCall& call) override;
    // Denial after preview armed the guard: drop the armed state so the
    // next legitimate call is not refused by the stale snapshot.
    void disarm(const ToolCall& call) override;

  private:
    FileToolsConfig config_;
    // Last preview snapshot per path: content the user approved writing
    // against. §5.3 race guard -- call() refuses when the on-disk file
    // differs from the previewed old content.
    std::map<std::string, std::string> armed_previews_;
};

// Strips C0 control characters (except \n and \t) and ANSI escape
// sequences from untrusted tool text before it reaches the self-drawn
// TUI, whose frame model assumes plain lines (design 008 section 5.4).
// Pure.
std::string sanitize_display(std::string_view text);

// v3 git tool surface (design 009 section 5.2, ADR 008 accepted
// 2026-11-05): read-only git_status/git_diff/git_log plus write-side
// git_add/git_commit following the unified approval tiers. NO
// push/remote/reset/rebase tools exist: remote operations are always
// human actions. Execution is fixed-argv `git -C <workspace> --no-pager`
// via run_subprocess (model text only ever lands in dedicated argv
// slots, never a shell). Liveness (git binary + repository) is probed
// lazily on first list()/call() -- construction never spawns (cold-start
// guarantee, 008 5.5); a dead source drops its tools via alive()==false.
class GitRuntime final : public ToolRuntime {
  public:
    explicit GitRuntime(std::filesystem::path workspace, std::size_t timeout_seconds = 30);

    std::vector<ToolDefinition> list() override;
    ToolResult call(const ToolCall& call, const ShouldAbort& should_abort) override;
    // git_commit previews the would-be commit: file list + diff stat
    // (computed read-only at approval time).
    std::string preview(const ToolCall& call) override;
    bool alive() const override;

  private:
    void ensure_probed() const;
    ToolResult run_git(const std::vector<std::string>& args, const ShouldAbort& should_abort) const;
    std::filesystem::path workspace_;
    std::size_t timeout_seconds_;
    mutable bool probed_ = false;
    mutable bool alive_ = false;
};

// Splits one `git status --porcelain=v1` line into {change letters,
// path}. Pure, exposed for tests. Returns false on unparseable input.
bool parse_porcelain_line(std::string_view line, std::string* status, std::string* path);

// Build the session's tool registry from merged config (design 008 5.3):
// builtin run_bash first, then the v3 FileRuntime (workspace-bounded file
// tools, design 009 5.1), then one McpClient per configured server routed
// through `factory` (nullptr -> default transport factory). Servers start
// lazily inside the client; this function itself never spawns anything
// (cold-start guarantee, 008 5.5). `workspace` empty = process cwd.
std::unique_ptr<ToolRegistry> build_tool_registry(const AppConfig& config, std::string* note);

} // namespace mini_agent
