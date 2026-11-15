#pragma once

#include "mini_agent/chat.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini_agent {

// 017 (design 017 §2.1): provider-billed anchor for the context reading.
// baseline_msg_count = number of messages the billed request contained
// (at turn commit: chat size minus the final assistant reply, which was
// not part of the last request); input_tokens = that request's
// prompt_tokens. Invalidated by the owner on any prefix change
// (Chat::replace via /compact, session load/switch) -- the index form of
// pi's timestamp invalidation, adapted to our timestamp-less Message.
struct UsageAnchor {
    std::size_t baseline_msg_count = 0;
    long long input_tokens = 0;
};

// 017: per-class token estimates (character-class heuristic, same
// estimator as the v4 fallback -- never a second estimator).
struct ContextBreakdown {
    long long system = 0;
    long long conversation = 0; // user + assistant (incl. tool_calls)
    long long tool_output = 0;  // role=Tool messages
};

struct ContextReading {
    long long used = 0;
    bool estimated = false; // true when no valid anchor (full estimate)
    bool exceeded = false;  // synthesized after an overflow rejection
    ContextBreakdown breakdown;
};

// 017 (design 017 §2.1): THE single reading function -- status row,
// /context and the pressure hint all consume this, never re-derive.
// Anchor valid (non-zero tokens, baseline within range): used = billed
// input + estimated tail after the baseline. Otherwise full estimate.
// exceeded_tokens > 0 (overflow synthesis, Zed-style) wins over both.
ContextReading context_reading(const Chat& chat, const std::optional<UsageAnchor>& anchor,
                               long long exceeded_tokens = 0);

// 017 (design 017 §2.1): pressure hint threshold = window - max(15% of
// window, 16384). Returns 0 (hint disabled) when window <= 0 or the
// reserve would swallow the whole window (tiny-window guard).
long long pressure_threshold(long long window);

// 017 (design 017 §2.2): first message index of the retained tail.
// Always keeps the LAST whole turn (last User message onward); extends
// backwards over whole earlier turns while the estimated tail stays
// within budget_tokens. The cut is always a User boundary, so tool
// call/result pairs are never orphaned (pi hard constraint). No User
// message -> messages.size() (empty tail).
std::size_t find_retained_tail(const std::vector<Message>& messages, long long budget_tokens);

// 017 (design 017 §2.2): mechanical prune of old verbose tool output,
// no model call. The trailing protected_window messages stay byte-for-
// byte intact. In the region before it, Tool messages over the verbose
// cap become one-line notes; the newest occurrence of an identical
// content keeps its full body (Codewhale duplicate rule), older copies
// and non-duplicate verbose results are pruned.
struct PruneResult {
    std::vector<Message> messages;
    std::size_t pruned = 0;
};
PruneResult prune_tool_outputs(const std::vector<Message>& messages,
                               std::size_t protected_window);

// 017 (design 017 §2.2): does the summary carry the structured template
// (>= 2 distinct section headings, case-insensitive)? Gates the NUDGE
// retry; a prose-only reply is a failed summary, not a usable one.
bool summary_has_template(std::string_view text);

// 017 (design 017 §2.1): overflow-shaped text that is actually a rate
// limit / throttling rejection (pi NON_OVERFLOW discipline) -- checked
// BEFORE context_overflow_message so a 400 throttle never reads as a
// context overflow.
bool overflow_excluded(std::string_view text);

// 017: parse the billed request size out of Anthropic-style
// "prompt is too long: N tokens ..." rejections; nullopt for other
// wordings (callers fall back to the model window).
std::optional<long long> parse_overflow_tokens(std::string_view text);

} // namespace mini_agent