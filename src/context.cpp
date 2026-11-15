// 017 (design 017): pure context-management core -- the single reading
// function, pressure threshold, retained-tail cut, mechanical tool
// output prune, summary template gate and overflow exclusions. No I/O,
// no terminal state; unit-tested via agent_test.
#include "mini_agent/context.hpp"

#include "mini_agent/provider.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace mini_agent {
namespace {

// Same character-class estimator as the v4 fallback (provider_openai),
// applied per message; assistant tool_calls count their wire payload.
long long estimate_message(const Message& message) {
    long long tokens = estimate_context_tokens(message.content);
    for (const auto& call : message.tool_calls)
        tokens += estimate_context_tokens(call.name) + estimate_context_tokens(call.arguments_json);
    return tokens;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

constexpr std::size_t kPruneVerboseChars = 600;

} // namespace

ContextReading context_reading(const Chat& chat, const std::optional<UsageAnchor>& anchor,
                               long long exceeded_tokens) {
    ContextReading reading;
    for (const auto& message : chat.messages()) {
        const long long tokens = estimate_message(message);
        switch (message.role) {
        case Role::System:
            reading.breakdown.system += tokens;
            break;
        case Role::Tool:
            reading.breakdown.tool_output += tokens;
            break;
        default:
            reading.breakdown.conversation += tokens;
            break;
        }
    }
    if (exceeded_tokens > 0) {
        reading.used = exceeded_tokens;
        reading.exceeded = true;
        return reading;
    }
    const auto& messages = chat.messages();
    if (anchor && anchor->input_tokens > 0 && anchor->baseline_msg_count <= messages.size()) {
        long long tail = 0;
        for (std::size_t i = anchor->baseline_msg_count; i < messages.size(); ++i)
            tail += estimate_message(messages[i]);
        reading.used = anchor->input_tokens + tail;
        reading.estimated = false;
    } else {
        reading.used =
            reading.breakdown.system + reading.breakdown.conversation + reading.breakdown.tool_output;
        reading.estimated = true;
    }
    return reading;
}

long long pressure_threshold(long long window) {
    if (window <= 0)
        return 0;
    const long long reserve = std::max(window * 15 / 100, 16384LL);
    if (reserve >= window)
        return 0; // tiny window: the reserve would swallow it, hint off
    return window - reserve;
}

std::size_t find_retained_tail(const std::vector<Message>& messages, long long budget_tokens) {
    std::size_t last_user = messages.size();
    for (std::size_t i = messages.size(); i-- > 0;) {
        if (messages[i].role == Role::User) {
            last_user = i;
            break;
        }
    }
    if (last_user == messages.size())
        return messages.size();
    std::size_t cut = last_user; // the last whole turn is always retained
    long long total = 0;
    for (std::size_t i = cut; i < messages.size(); ++i)
        total += estimate_message(messages[i]);
    std::size_t next_user = last_user;
    for (std::size_t i = last_user; i-- > 0;) {
        if (messages[i].role != Role::User)
            continue;
        long long turn = 0;
        for (std::size_t j = i; j < next_user; ++j)
            turn += estimate_message(messages[j]);
        if (total + turn > budget_tokens)
            break;
        total += turn;
        cut = i;
        next_user = i;
    }
    return cut;
}

PruneResult prune_tool_outputs(const std::vector<Message>& messages,
                               std::size_t protected_window) {
    PruneResult out;
    out.messages = messages;
    const std::size_t n = messages.size();
    const std::size_t region_end = n > protected_window ? n - protected_window : 0;
    // Newest occurrence of a DUPLICATED verbose content keeps its full
    // body; older copies and non-duplicate verbose results become notes.
    std::map<std::string, std::size_t> newest;
    std::map<std::string, std::size_t> count;
    for (std::size_t i = 0; i < region_end; ++i) {
        if (messages[i].role == Role::Tool && messages[i].content.size() > kPruneVerboseChars) {
            newest[messages[i].content] = i;
            ++count[messages[i].content];
        }
    }
    for (std::size_t i = 0; i < region_end; ++i) {
        auto& message = out.messages[i];
        if (message.role != Role::Tool || message.content.size() <= kPruneVerboseChars)
            continue;
        const bool duplicate = count[message.content] > 1;
        if (duplicate && newest[message.content] == i)
            continue; // freshest full body of a duplicated result
        message.content = "[pruned tool output: " + std::to_string(message.content.size()) +
                          " bytes" + (duplicate ? ", duplicate of a newer result" : "") + "]";
        ++out.pruned;
    }
    return out;
}

bool summary_has_template(std::string_view text) {
    const std::string lower = lowered(text);
    static constexpr std::string_view kHeadings[] = {
        "## goal", "## constraints", "## progress", "## key decisions",
        "## next", "## pitfalls", "## critical",
    };
    int hits = 0;
    for (const auto heading : kHeadings) {
        if (lower.find(heading) != std::string::npos)
            ++hits;
    }
    return hits >= 2;
}

bool overflow_excluded(std::string_view text) {
    const std::string lower = lowered(text);
    return lower.find("rate limit") != std::string::npos ||
           lower.find("too many requests") != std::string::npos ||
           lower.find("throttl") != std::string::npos;
}

std::optional<long long> parse_overflow_tokens(std::string_view text) {
    const std::string lower = lowered(text);
    // Anthropic: "prompt is too long: N tokens > M maximum";
    // OpenAI/LiteLLM: "... your messages resulted in N tokens."
    for (const std::string_view prefix : {"prompt is too long: ", "resulted in "}) {
        const std::size_t pos = lower.find(prefix);
        if (pos == std::string::npos)
            continue;
        std::size_t i = pos + prefix.size();
        long long value = 0;
        bool any = false;
        while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
            value = value * 10 + (lower[i] - '0');
            any = true;
            ++i;
        }
        if (any && value > 0)
            return value;
    }
    return std::nullopt;
}

} // namespace mini_agent