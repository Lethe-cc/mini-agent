#pragma once

// v4.17 (design 007 section 5.6 rule 1, 008 section 5.4): the worker<->UI
// handoff state of one TUI run, extracted from tui.cpp so it can be
// unit-tested without a pty (agent_test drives both sides directly;
// design 008 open item D4). tui.cpp keeps owning the frame loop and the
// rendering; everything crossing the thread boundary lives here.
//
// Thread discipline is unchanged:
//  - deltas: worker appends, main drains per frame (internal mutex).
//  - tool events: worker pushes under one mutex, main swaps out.
//  - approvals: fixed slot + sequence protocol (no pointers move);
//    the worker publishes then waits, the main thread answers with the
//    UI key semantics. Timeouts are decided by the CALLER (tui.cpp) so
//    tests can inject short budgets.
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mini_agent/chat.hpp"

namespace mini_agent::tui {

// One decided tool invocation parked for the main thread (worker -> UI
// handoff: rounds are committed to history only on the main thread, same
// rule as deltas -- 5.6 rule 1).
struct ToolRound {
    std::string tool_name;
    std::string arguments_json;
    bool executed = false;
    bool is_error = false;
    long long elapsed_ms = 0;
    std::size_t result_bytes = 0;
    bool approval_timeout = false;
    // v5 (design 011): post-execution human preview staged by the worker
    // (applied diff for file writes / body for preview_file). Plain text,
    // unsanitized; the UI thread styles it when committing the block.
    std::string result_preview;
    // Set when the preview should render as Markdown (preview_file on a
    // .md/.markdown file) rather than as a diff.
    bool preview_is_markdown = false;
    // v5 (design 011 5.6): when non-empty, the plain body highlights with
    // this language lexer (preview_file on a code file).
    std::string preview_lang;
};

// Auto-deny budget for one approval request (design 008 section 5.4): the
// worker waits this long for a keypress before denying. The last third of
// the budget renders as a countdown in the status line (v4.17), so a user
// returning to the terminal still gets a chance to answer.
constexpr long long kApprovalTimeoutS = 120;
constexpr long long kApprovalWarnAtS = 90;

// Outcome of TurnBridge::wait_approval.
enum class ApprovalWait { Answer, Timeout, Aborted };

// 014 (design 014 §5): one pre-stream provider retry, parked by the worker
// for the main thread to log. The logger itself is main-thread-owned (/log
// may reassign it mid-turn), so retry events cross the boundary as data,
// never as a logger pointer.
struct RetryEvent {
    int attempt = 0;     // 1-based
    int http_status = 0; // 0 = network error
    int wait_ms = 0;
};

class TurnBridge {
  public:
    // ---- streaming deltas (worker appends, main reads per frame) ----
    void append_delta(std::string_view delta) {
        std::lock_guard<std::mutex> lock(deltas_mutex_);
        deltas_.append(delta);
    }
    std::string deltas_snapshot() {
        std::lock_guard<std::mutex> lock(deltas_mutex_);
        return deltas_;
    }

    // ---- tool event queue ----
    void push_tool_event(ToolRound round) {
        std::lock_guard<std::mutex> lock(tool_mutex_);
        tool_events_.push_back(std::move(round));
    }
    std::vector<ToolRound> drain_tool_events() {
        std::vector<ToolRound> batch;
        std::lock_guard<std::mutex> lock(tool_mutex_);
        batch.swap(tool_events_);
        return batch;
    }
    void set_running_tool(std::string line) {
        std::lock_guard<std::mutex> lock(tool_mutex_);
        running_tool_ = std::move(line);
    }
    // Atomic "tool finished": clears the running row and parks the
    // completed round in one lock hold, so a frame between the two never
    // shows the call as vanished (matches the old inline mutex block).
    void finish_running_tool(ToolRound round) {
        std::lock_guard<std::mutex> lock(tool_mutex_);
        running_tool_.clear();
        tool_events_.push_back(std::move(round));
    }
    std::string running_tool() {
        std::lock_guard<std::mutex> lock(tool_mutex_);
        return running_tool_;
    }

    // ---- 014: provider retry events (worker pushes, main drains+logs) ----
    void push_retry_event(RetryEvent event) {
        std::lock_guard<std::mutex> lock(tool_mutex_);
        retry_events_.push_back(event);
    }
    std::vector<RetryEvent> drain_retry_events() {
        std::vector<RetryEvent> batch;
        std::lock_guard<std::mutex> lock(tool_mutex_);
        batch.swap(retry_events_);
        return batch;
    }

    // ---- per-turn reset (start_turn, main thread, worker joined) ----
    void reset_turn() {
        {
            std::lock_guard<std::mutex> lock(deltas_mutex_);
            deltas_.clear();
        }
        std::lock_guard<std::mutex> lock(tool_mutex_);
        tool_events_.clear();
        running_tool_.clear();
        retry_events_.clear();
    }

    // ---- approval protocol (fixed slot + sequence, 008 section 5.4) ----
    bool approval_pending() const {
        return approval_slot_.request_seq.load() != approval_slot_.answer_seq.load();
    }
    // Worker side: fill call/preview + countdown stamp, publish, return the
    // sequence number the answer must carry.
    unsigned long long publish_approval(const ToolCall& call, std::string preview) {
        std::lock_guard<std::mutex> lock(approval_slot_.mutex);
        approval_slot_.call = call;
        approval_slot_.preview = std::move(preview);
        approval_slot_.published_at_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        const unsigned long long seq = approval_slot_.request_seq.load() + 1;
        approval_slot_.request_seq.store(seq); // publish (call set first)
        return seq;
    }
    // Main-thread keypress answer (1 allow, 2 deny, 3 allow-session).
    void answer_approval(int answer) {
        approval_slot_.answer.store(answer);
        approval_slot_.answer_seq.store(approval_slot_.request_seq.load());
    }
    // Seconds elapsed since the current request was published (frame-loop
    // countdown input; 0 before the first publish).
    long long approval_waited_s() const {
        const long long pub = approval_slot_.published_at_ms.load();
        if (pub <= 0)
            return 0;
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        return (now - pub) / 1000;
    }
    // Snapshot of the pending request for rendering (main thread). Both
    // getters take the payload mutex; a frame never sees call/preview from
    // two different requests.
    ToolCall approval_call() {
        std::lock_guard<std::mutex> lock(approval_slot_.mutex);
        return approval_slot_.call;
    }
    std::string approval_preview() {
        std::lock_guard<std::mutex> lock(approval_slot_.mutex);
        return approval_slot_.preview;
    }
    // Worker side: block until seq is answered, the abort flag raises, or
    // the (injectable, testable) budget runs out. On Timeout the caller
    // must park a denial ToolRound itself and treat the call as denied --
    // same as tui.cpp does with the 120 s budget.
    template <typename AbortFn>
    ApprovalWait wait_approval(unsigned long long seq, long long timeout_seconds,
                               const AbortFn& abort_requested) {
        const auto wait_start = std::chrono::steady_clock::now();
        while (approval_slot_.answer_seq.load() != seq) {
            if (abort_requested())
                return ApprovalWait::Aborted;
            const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - wait_start)
                                    .count();
            if (waited >= timeout_seconds)
                return ApprovalWait::Timeout;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return ApprovalWait::Answer;
    }
    // Latest answer value for `seq` after wait_approval returns. An
    // UNANSWERED sequence always reads as deny (2): the original inline
    // protocol loaded the raw atomic on timeout, which could return a
    // STALE allow from the previous request (latent bug, caught by the
    // v4.17 unit test).
    int approval_answer(unsigned long long seq) const {
        if (approval_slot_.answer_seq.load() != seq)
            return 2;
        return approval_slot_.answer.load();
    }

  private:
    struct Slot {
        std::mutex mutex; // guards `call`/`preview` payload writes
        ToolCall call;    // valid between request and answer
        // v3 (design 009 5.3): pre-computed diff preview for write-side
        // tools, written under the same mutex BEFORE request_seq
        // publishes, so the UI thread never sees a pending approval
        // without its preview settled.
        std::string preview;
        std::atomic<unsigned long long> request_seq{0};
        std::atomic<unsigned long long> answer_seq{0};
        std::atomic<int> answer{0}; // 1 allow, 2 deny, 3 allow-session
        // Steady-clock ms of the publish that opened this request (worker
        // writes before request_seq, main reads per frame for the
        // countdown).
        std::atomic<long long> published_at_ms{0};
    } approval_slot_;

    std::mutex deltas_mutex_;
    std::string deltas_;
    std::mutex tool_mutex_;
    std::vector<ToolRound> tool_events_;
    std::string running_tool_;
    std::vector<RetryEvent> retry_events_; // 014, same mutex as tool events
};

} // namespace mini_agent::tui
