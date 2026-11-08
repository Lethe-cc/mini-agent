#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/chat.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/tools.hpp"

namespace mini_agent {

// v2 loop options (design 008 section 5.2). All-default TurnOptions ==
// exact v1 single-shot behavior (regression hook, section 7).
struct TurnOptions {
    // nullptr or empty registry -> no tools are advertised at all.
    ToolRegistry* tools = nullptr;
    std::size_t max_tool_rounds = 8;
    // Asked once per invocation before execution. Empty approver + non-
    // empty registry == auto-allow (CLI `ask --tools auto` and tests);
    // the interactive default is an approver that prompts (ADR 006).
    ToolApprover approve;
    // UI hook: fires after each tool call is decided. `executed` false =
    // denied by the approver (result carries the refusal note).
    std::function<void(const ToolCall&, const ToolResult&, bool executed)> on_tool;
    // v4.11 (design 010 §21): fires once per call AFTER approval and
    // BEFORE execution -- lets the TUI panel show a running tool. Not
    // fired for denied calls (they go straight to on_tool).
    std::function<void(const ToolCall&)> on_tool_start;
    // Abort hook applied to tool execution as well (kills bash group,
    // stops MCP calls between rounds). Null treated as never-abort.
    ShouldAbort should_abort;
    // v4 (design 010): optional out-pointer accumulating provider usage
    // across the turn's round trips. Semantics chosen for the status bar:
    // prompt_tokens = the LAST round's prompt size (the live context
    // reading), completion_tokens/total_tokens = sums over all rounds
    // (workload/cost reading). Only written when the whole turn succeeded
    // (same commit boundary as the chat history); zero when no provider
    // in the chain reported usage.
    TokenUsage* usage_out = nullptr;
    // v4.2 (design 010 §11): optional out-pointer for reasoning evidence
    // OBSERVED in the responses (effort echo / reasoning tokens / content
    // deltas). Written on success only; `seen` ORs across rounds, the
    // echoed effort keeps the last non-empty value.
    ActualReasoning* reasoning_out = nullptr;
};

// Runs one user turn, possibly several provider round-trips when the model
// requests tools. Appends the user message, every intermediate assistant
// tool-call message and Tool result, and the final assistant reply to
// `chat` ONLY when the whole turn succeeded: a ProviderError (or abort)
// anywhere leaves the stored history at the previous turn boundary, even
// though executed tools keep their system side effects (design 007 section
// 5 + 008 section 4/6). Returns the final assistant text.
std::string run_turn(
    Chat& chat, Provider& provider, std::string_view user_input,
    const std::function<void(std::string_view)>& on_delta,
    const ShouldAbort& should_abort = [] { return false; }, const TurnOptions& options = {});

} // namespace mini_agent
