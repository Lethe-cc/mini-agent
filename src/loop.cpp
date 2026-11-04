#include "mini_agent/loop.hpp"

#include <chrono>
#include <set>

namespace mini_agent {
namespace {

// One provider round-trip inside the turn: returns the completion and
// stages the assistant message (text + any tool_calls) into `staging`.
CompletionResult ask_provider(Chat& staging, Provider& provider, const std::string& tools_json,
                              const std::function<void(std::string_view)>& on_delta,
                              const ShouldAbort& should_abort) {
    auto result = provider.complete_chat(staging, tools_json, on_delta, should_abort);
    Message assistant;
    assistant.role = Role::Assistant;
    assistant.content = result.text;
    assistant.tool_calls = result.tool_calls;
    staging.add(std::move(assistant));
    return result;
}

ToolResult execute_one(ToolRegistry& registry, const ToolCall& call,
                       const ShouldAbort& should_abort) {
    const auto started = std::chrono::steady_clock::now();
    auto finish = [&](ToolResult result) {
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        return result;
    };
    auto* runtime = registry.find(call.name);
    if (!runtime) {
        // Unknown tool: fold into an error result so the model can
        // self-correct instead of crashing the loop (008 5.2).
        return finish(ToolResult{"no such tool: " + call.name + " (check /tools listing)", true});
    }
    try {
        auto result = runtime->call(call, should_abort);
        result.text = cap_tool_output(result.text, BuiltinRuntime::kOutputLimitBytes);
        return finish(std::move(result));
    } catch (const ProviderError&) {
        throw; // transport-level errors keep their kind
    } catch (const std::exception& e) {
        return finish(ToolResult{std::string("tool runtime error: ") + e.what(), true});
    }
}

} // namespace

std::string run_turn(Chat& chat, Provider& provider, std::string_view user_input,
                     const std::function<void(std::string_view)>& on_delta,
                     const ShouldAbort& should_abort, const TurnOptions& options) {
    Message user;
    user.role = Role::User;
    user.content.assign(user_input);

    // Outgoing history = stored history + this user message. The stored
    // chat is only mutated once the whole turn (all tool rounds included)
    // succeeded, so a failed turn leaves history at the previous turn
    // boundary (design 007 section 5 + 008 section 4). A context
    // compressor would choose what `outgoing` holds here without ever
    // touching `chat` (007 5.5).
    Chat staging;
    for (const auto& message : chat.messages()) {
        staging.add(Message{message});
    }
    staging.add(Message{user});

    const bool tools_enabled = options.tools != nullptr && !options.tools->empty();
    const std::string tools_json = tools_enabled ? options.tools->to_openai_tools_json() : "";
    const ShouldAbort abort = options.should_abort ? options.should_abort : should_abort;

    std::set<std::string> session_allowed; // Approval::AllowSession scope
    std::string final_text;

    for (std::size_t round = 0; round <= options.max_tool_rounds; ++round) {
        const bool last_round = round == options.max_tool_rounds;
        std::string tools_for_round = (tools_enabled && !last_round) ? tools_json : std::string();
        if (tools_enabled && last_round) {
            // Budget exhausted: ask for a final summary WITHOUT tools. The
            // note is staging-only and is filtered out at commit time
            // (008 5.2 max-rounds rule).
            Message note;
            note.role = Role::System;
            note.content =
                "Tool round limit reached for this turn. Reply with your best summary now; "
                "do not request more tools.";
            staging.add(std::move(note));
        }
        auto result = ask_provider(staging, provider, tools_for_round, on_delta, abort);
        // Without a registry nothing can execute: keep exact v1 semantics
        // and treat any tool_calls the model hallucinated as ignorable.
        if (!tools_enabled || !result.has_tool_calls() || last_round) {
            final_text = std::move(result.text);
            break;
        }
        // v2.0 executes calls sequentially (008 section 2 non-goal).
        for (const auto& call : result.tool_calls) {
            bool executed = false;
            ToolResult tool_result;
            const bool pre_allowed = session_allowed.count(call.name) != 0;
            if (options.approve && !pre_allowed) {
                const Approval decision = options.approve(call);
                if (decision == Approval::AllowSession) {
                    session_allowed.insert(call.name);
                }
                if (decision == Approval::Deny) {
                    tool_result = ToolResult{"user denied this tool invocation", true};
                } else {
                    executed = true;
                    tool_result = execute_one(*options.tools, call, abort);
                }
            } else {
                executed = true;
                tool_result = execute_one(*options.tools, call, abort);
            }
            if (abort()) {
                throw ProviderError(ProviderErrorKind::Cancelled, "request cancelled");
            }
            if (options.on_tool) {
                options.on_tool(call, tool_result, executed);
            }
            Message tool_message;
            tool_message.role = Role::Tool;
            tool_message.content = std::move(tool_result.text);
            tool_message.tool_call_id = call.id;
            staging.add(std::move(tool_message));
        }
    }

    // Commit: the whole turn lands in the stored chat at once (008 5.2).
    // staging layout: [original prefix][user][assistant/tool ...], where
    // the only System message that can appear after the prefix is the
    // transient max-rounds note -- never persisted.
    const std::size_t prefix = chat.messages().size();
    chat.add(std::move(user));
    for (std::size_t i = prefix + 1; i < staging.messages().size(); ++i) {
        const auto& message = staging.messages()[i];
        if (message.role == Role::System)
            continue; // transient limit note
        chat.add(Message{message});
    }
    return final_text;
}

} // namespace mini_agent
