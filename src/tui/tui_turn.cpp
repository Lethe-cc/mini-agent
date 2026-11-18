// TUI turn worker (design 019 P4 / §8). Body moved verbatim from the
// run_loop thread closure; shared control state via TurnState&.
#include "tui_turn.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

#include "mini_agent/highlight.hpp"
#include "mini_agent/tui.hpp"

namespace mini_agent::tui {

void run_turn_worker(TurnState& S, const AppConfig& turn_config, Chat& chat, std::string user_msg) {
    auto provider_config = turn_config.to_provider();
    provider_config.reasoning_live = &S.reasoning_live; // v4.11 panel
    // 014 (design 014 §5): metadata-only retry notification; never
    // on_delta (reply text/history stay free of retry noise).
    provider_config.on_retry = [&S](int attempt, int http_status, int wait_ms) {
        S.bridge.push_retry_event(RetryEvent{attempt, http_status, wait_ms});
    };
    OpenAiProvider provider(std::move(provider_config));
    // v4.17: the old second deep copy (`Chat outgoing`) was dead
    // code -- run_turn builds and commits its own `turn_chat`
    // below; one snapshot per turn is the thread boundary.
    TurnResult r;
    // Snapshot of the shared control state for this turn's tools
    // (thread-boundary discipline mirrors turn_config; the TUI
    // rebuilds the registry before every start_turn, so mutations
    // of `S.tool_registry` here are serialized by the worker join).
    ToolRegistry* registry = S.tool_registry.get();
    TurnOptions turn_options;
    turn_options.tools = registry;
    turn_options.max_tool_rounds = S.max_tool_rounds;
    turn_options.should_abort = [&] { return S.abort_requested.load() || S.approval_abort.load(); };
    turn_options.approve = [&](const ToolCall& call) -> Approval {
        const bool auto_run = S.approval_policy == "off" ||
                              (S.approval_policy == "allowlist" &&
                               std::find(S.config_snapshot.tool_allowlist.begin(),
                                         S.config_snapshot.tool_allowlist.end(),
                                         call.name) != S.config_snapshot.tool_allowlist.end());
        if (auto_run || S.session_allowed_tools.count(call.name) != 0)
            return Approval::Allow;
        unsigned long long seq = 0;
        // v3: compute the diff preview on the worker thread
        // (registry preview honors the same single-owner
        // contract as list/call) and arm the runtime's race
        // guard -- both before publish. publish_approval stamps
        // the countdown source and publishes request_seq (v4.17:
        // protocol lives in TurnBridge).
        seq = S.bridge.publish_approval(call, registry ? registry->preview(call) : std::string());
        const auto wait = S.bridge.wait_approval(seq, kApprovalTimeoutS,
                                                 [&] { return S.abort_requested.load(); });
        if (wait == ApprovalWait::Timeout) { // never block the worker forever
            ToolRound timeout_round;
            timeout_round.tool_name = call.name;
            timeout_round.arguments_json = call.arguments_json;
            timeout_round.executed = false;
            timeout_round.is_error = true;
            timeout_round.approval_timeout = true;
            S.bridge.push_tool_event(std::move(timeout_round));
        }
        const int answer = S.bridge.approval_answer(seq);
        switch (answer) {
        case 1:
            return Approval::Allow;
        case 3: {
            std::lock_guard<std::mutex> lock(S.session_allowed_mutex);
            S.session_allowed_tools.insert(call.name);
            return Approval::AllowSession;
        }
        default:
            // v3: denial must release any preview-armed race guard
            // (worker owns the registry; preview() armed it here).
            if (registry)
                registry->disarm(call);
            return Approval::Deny;
        }
    };
    turn_options.on_tool_start = [&](const ToolCall& call) {
        // v4.11: panel shows the call as running until on_tool
        // lands (approval-gated calls fire this only after the
        // approve decision, denied calls never fire it).
        S.bridge.set_running_tool("[tool] " + sanitize_display(call.name) +
                                  (tool_arg_summary(call.arguments_json).empty()
                                       ? std::string()
                                       : " · " + tool_arg_summary(call.arguments_json)) +
                                  " [running]");
    };
    turn_options.on_tool = [&](const ToolCall& call, const ToolResult& tool_result, bool executed) {
        ToolRound round;
        round.tool_name = call.name;
        round.arguments_json = call.arguments_json;
        round.executed = executed;
        round.is_error = tool_result.is_error;
        round.elapsed_ms = tool_result.elapsed_ms;
        round.result_bytes = tool_result.text.size();
        // v5 (design 011): stage the human preview on the worker
        // (loop) thread right after execution -- the registry's
        // result_preview shares call()'s single-owner contract.
        if (executed && !tool_result.is_error && registry) {
            round.result_preview = registry->result_preview(call, tool_result);
            if (call.name == "preview_file") {
                // .md/.markdown bodies render; others show verbatim.
                // The path lives in the args; parse it for the
                // extension (staged body carries no name).
                const auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
                std::string path =
                    args.is_object() && args.contains("path") && args["path"].is_string()
                        ? args["path"].get<std::string>()
                        : std::string();
                std::string lower = path;
                for (auto& c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto ends = [&](std::string_view suf) {
                    return lower.size() >= suf.size() &&
                           lower.compare(lower.size() - suf.size(), suf.size(), suf) == 0;
                };
                round.preview_is_markdown = ends(".md") || ends(".markdown");
                // v5 5.6: non-markdown recognized code files
                // highlight; markdown takes precedence above.
                if (!round.preview_is_markdown)
                    round.preview_lang = hl::language_for_path(path);
            }
        }
        S.bridge.finish_running_tool(std::move(round));
    };
    // v4: run_turn writes here only on a successful turn; the
    // worker copies it into `S.result` (join-then-read discipline).
    TokenUsage turn_usage;
    turn_options.usage_out = &turn_usage;
    ActualReasoning turn_reasoning; // v4.2
    turn_options.reasoning_out = &turn_reasoning;
    try {
        // v4.17 (copy-on-write thread boundary): run_turn takes
        // the stored chat by reference and commits the whole turn
        // to it ONLY on success -- the loop's internal staging
        // copy is the error isolation (5.6 rule: failures never
        // reach history; 008 section 4). The worker's former deep
        // copy of the entire history was redundant defense and
        // made every turn O(chat) twice; the CLI path already
        // passes chat directly. Main-thread reads of the
        // chat during the turn are gated exactly as before: the
        // only writer window is the commit at turn end, and the
        // same-turn success write happened in the old design too.
        r.reply = run_turn(
            chat, provider, user_msg, [&](std::string_view d) { S.bridge.append_delta(d); },
            [&] { return S.abort_requested.load(); }, turn_options);
        r.usage = turn_usage;
        r.reasoning = turn_reasoning;
        r.ok = true;
    } catch (const ProviderError& e) {
        r.error = e.what();
        r.http_status = e.http_status();
        r.network_error = e.kind() == ProviderErrorKind::Network;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    if (S.abort_requested.load())
        r.aborted = true;
    S.result = std::move(r);
    S.turn_done = true;
}

} // namespace mini_agent::tui
