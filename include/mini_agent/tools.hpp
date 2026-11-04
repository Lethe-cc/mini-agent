#pragma once

#include <functional>
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

    explicit BuiltinRuntime(std::size_t default_timeout_seconds = 30)
        : default_timeout_seconds_(default_timeout_seconds) {
    }

    std::vector<ToolDefinition> list() override;
    ToolResult call(const ToolCall& call, const ShouldAbort& should_abort) override;

  private:
    std::size_t default_timeout_seconds_;
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

// Truncates tool output keeping head and tail halves with a marker
// (design 008 section 5.2). Pure and tested.
std::string cap_tool_output(std::string_view text, std::size_t limit_bytes);

// Strips C0 control characters (except \n and \t) and ANSI escape
// sequences from untrusted tool text before it reaches the self-drawn
// TUI, whose frame model assumes plain lines (design 008 section 5.4).
// Pure.
std::string sanitize_display(std::string_view text);

// Build the session's tool registry from merged config (design 008 5.3):
// builtin run_bash first, then one McpClient per configured server routed
// through `factory` (nullptr -> default transport factory). Servers start
// lazily inside the client; this function itself never spawns anything
// (cold-start guarantee, 008 5.5).
std::unique_ptr<ToolRegistry> build_tool_registry(const AppConfig& config, std::string* note);

} // namespace mini_agent
