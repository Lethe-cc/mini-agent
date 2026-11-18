#pragma once
// Internal TUI turn/approval cluster (design 019 P4 / §8 TuiState first slice).
// TurnState owns the shared turn/approval control state; run_loop aliases its
// members so existing main-thread code is unchanged. The worker body lives in
// tui_turn.cpp and only reads snapshots + writes bridge/result (join-then-read).
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "mini_agent/config.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/tui_bridge.hpp"

namespace mini_agent::tui {

struct TurnResult {
    bool ok = false;
    bool aborted = false;
    std::string reply;
    std::string error;
    TokenUsage usage;
    ActualReasoning reasoning;
    int http_status = 0;
    bool network_error = false;
};

struct TurnState {
    TurnBridge bridge;
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> turn_done{false};
    TurnResult result;
    std::atomic<bool> reasoning_live{false};
    std::atomic<bool> approval_abort{false};
    std::set<std::string> session_allowed_tools;
    std::mutex session_allowed_mutex;
    AppConfig config_snapshot;
    std::size_t max_tool_rounds = 0;
    std::unique_ptr<ToolRegistry> tool_registry;
    std::string approval_policy;
};

// Worker body: runs one turn off-thread. Writes S.result / S.turn_done only;
// screen/logger stay on the main thread. `chat` is the stored session chat
// (run_turn commits to it only on success).
void run_turn_worker(TurnState& S, const AppConfig& turn_config, Chat& chat, std::string user_msg);

} // namespace mini_agent::tui