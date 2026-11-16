// Pure OpenAI-wire parsers/estimators (design 019 P1): no curl, no IO.
// Definitions moved verbatim from provider_openai.cpp; declarations live
// in mini_agent/provider.hpp so the TUI/CLI/tests share one contract.
#include "mini_agent/provider.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini_agent {
bool retryable_provider_error(ProviderErrorKind kind, int http_status) {
    if (kind == ProviderErrorKind::Network)
        return true; // curl failures short of cancellation are transient candidates
    if (kind != ProviderErrorKind::HttpStatus)
        return false; // Protocol/Cancelled: never retry
    switch (http_status) {
    case 408: // request timeout
    case 429: // rate limited
    case 500:
    case 502:
    case 503:
    case 504:
        return true;
    default:
        return false; // 401/403 key rejection, 400 bad request, ...
    }
}

bool context_overflow_message(std::string_view text) {
    // 015 (design 015 §4B): narrow pattern set, same philosophy as the
    // probe parser (010 §13) -- gateway wording is not a contract, so an
    // unrecognized message yields false, never a guess.
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    });
    static constexpr std::string_view kPatterns[] = {
        "context_length_exceeded", "maximum context length", "context length",
        "reduce the length",       "range of input length",  "too many tokens",
    };
    for (const auto pattern : kPatterns) {
        if (lower.find(pattern) != std::string::npos)
            return true;
    }
    return false;
}

std::optional<std::vector<std::string>> parse_models_json(std::string_view body,
                                                          std::string* error) {
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "models response is not a JSON object";
        return std::nullopt;
    }
    const auto data = parsed.find("data");
    if (data == parsed.end() || !data->is_array()) {
        if (error)
            *error = "models response missing data array";
        return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& entry : *data) {
        if (!entry.is_object())
            continue; // skip malformed entries
        const auto id = entry.find("id");
        if (id != entry.end() && id->is_string())
            ids.push_back(id->get<std::string>());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

long long estimate_context_tokens(std::string_view text) {
    // Coarse character-class estimate (v4 fallback when a gateway sends no
    // usage chunk). Walk UTF-8 codepoints: ASCII letters/digits/punct run
    // at ~4 chars/token, CJK & fullwidth at ~1.5 chars each, everything
    // else ~3. Never exact -- callers must mark results with `~`.
    long long ascii_chars = 0;
    long long cjk_codepoints = 0;
    long long other_codepoints = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if (b0 < 0x80) {
            ++ascii_chars;
        } else {
            if ((b0 >> 5) == 0b110)
                len = 2;
            else if ((b0 >> 4) == 0b1110)
                len = 3;
            else if ((b0 >> 3) == 0b11110)
                len = 4;
            // U+4E00..U+9FFF, U+3040..U+30FF (kana), U+AC00..U+D7AF (hangul),
            // U+FF00..U+FFEF (fullwidth): decode only 3-byte range cheaply.
            const bool likely_cjk = len == 3 && b0 >= 0xE3 && b0 <= 0xEF;
            if (likely_cjk)
                ++cjk_codepoints;
            else
                ++other_codepoints;
        }
        i += len;
    }
    const long long tokens = ascii_chars / 4 + cjk_codepoints + other_codepoints / 3;
    return tokens > 0 ? tokens : (text.empty() ? 0 : 1);
}

ProbeOutcome parse_context_probe_error(std::string_view body) {
    ProbeOutcome outcome;
    // Sanity bounds: real windows live between 1k and ~200M tokens; a
    // parse outside this range means we matched a wrong number (a byte
    // count, a retry-after, ...), so prefer no answer over a wrong one.
    auto in_range = [](long long n) {
        constexpr long long lo = 1000, hi = 200'000'000;
        return n >= lo && n <= hi;
    };
    // The message may arrive as a JSON error envelope; extract the human
    // text first so numeric JSON fields (an echoed "max_tokens": 1e9
    // request value) can never be mistaken for the limit. Fall back to
    // the raw body for non-JSON error pages.
    std::string text;
    {
        auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_discarded()) {
            if (auto err = parsed.find("error"); err != parsed.end() && err->is_object()) {
                if (auto m = err->find("message"); m != err->end() && m->is_string())
                    text = m->get<std::string>();
            }
        }
        if (text.empty())
            text.assign(body);
    }
    const auto digits_after = [&text, &in_range](std::size_t from) -> std::optional<long long> {
        const auto s = text.find_first_of("0123456789", from);
        if (s == std::string::npos)
            return std::nullopt;
        std::size_t e = s;
        long long value = 0;
        while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e]))) {
            value = value * 10 + (text[e] - '0');
            ++e;
        }
        if (!in_range(value))
            return std::nullopt;
        return value;
    };
    // 1) "maximum context length is 32768 tokens" (vLLM, OpenAI): TOTAL
    //    window -- the correct denominator for the ctx reading.
    if (auto pos = text.find("maximum context length is"); pos != std::string::npos) {
        if (auto value = digits_after(pos)) {
            outcome.kind = ProbeLimitKind::Window;
            outcome.value = *value;
            return outcome;
        }
    }
    // 2) "Range of <param> should be [1, N]" (DashScope family): the LAST
    //    digit run inside the brackets is the upper bound (the first is
    //    the lower 1). <param> decides the KIND: an input-length range is
    //    a usable window proxy; a max_tokens range is the OUTPUT cap and
    //    must never be stored as the context window (maintainer data:
    //    qwen3.8-flash input 983616 vs output cap 131072).
    if (auto pos = text.find("Range of"); pos != std::string::npos) {
        const auto open = text.find('[', pos);
        const auto close = open == std::string::npos ? std::string::npos : text.find(']', open);
        if (open != std::string::npos && close != std::string::npos) {
            std::optional<long long> last;
            for (std::size_t i = open + 1; i < close;) {
                if (std::isdigit(static_cast<unsigned char>(text[i]))) {
                    long long value = 0;
                    while (i < close && std::isdigit(static_cast<unsigned char>(text[i]))) {
                        value = value * 10 + (text[i] - '0');
                        ++i;
                    }
                    if (in_range(value))
                        last = value;
                } else {
                    ++i;
                }
            }
            if (last) {
                const std::string param(text, pos + 8, open - (pos + 8));
                const bool output = param.find("max_tokens") != std::string::npos ||
                                    param.find("max_output") != std::string::npos ||
                                    param.find("output") != std::string::npos;
                const bool input = param.find("input") != std::string::npos ||
                                   param.find("prompt") != std::string::npos;
                if (output) {
                    outcome.kind = ProbeLimitKind::OutputCap;
                    outcome.value = *last;
                } else if (input) {
                    outcome.kind = ProbeLimitKind::InputCap;
                    outcome.value = *last;
                } // else ambiguous param name: stay Unknown (never guess)
                return outcome;
            }
        }
    }
    // 3) "max_tokens: 1000000 > 131072" (OpenAI): the per-request OUTPUT
    //    cap. On a tiny prompt that is NOT the window -- classify honestly.
    if (auto pos = text.find("max_tokens:"); pos != std::string::npos) {
        const auto gt = text.find('>', pos);
        if (gt != std::string::npos) {
            if (auto value = digits_after(gt)) {
                outcome.kind = ProbeLimitKind::OutputCap;
                outcome.value = *value;
                return outcome;
            }
        }
    }
    return outcome; // Unknown, value 0
}

std::optional<std::vector<ModelCapabilities>> parse_models_meta_json(std::string_view body,
                                                                     std::string* error) {
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "models response is not a JSON object";
        return std::nullopt;
    }
    const auto data = parsed.find("data");
    if (data == parsed.end() || !data->is_array()) {
        if (error)
            *error = "models response missing data array";
        return std::nullopt;
    }
    std::vector<ModelCapabilities> caps;
    for (const auto& entry : *data) {
        if (!entry.is_object())
            continue;
        const auto id = entry.find("id");
        if (id == entry.end() || !id->is_string())
            continue; // same skip rule as parse_models_json
        ModelCapabilities model;
        model.id = id->get<std::string>();
        if (auto cl = entry.find("context_length");
            cl != entry.end() && cl->is_number_integer() && cl->get<long long>() > 0)
            model.context_length = cl->get<long long>();
        // OpenRouter shape: reasoning = {mandatory, default_enabled,
        // supported_efforts[], default_effort}. Any other shape (a bare
        // bool like some gateways use) only flips supports_reasoning.
        if (auto r = entry.find("reasoning"); r != entry.end()) {
            if (r->is_object()) {
                model.supports_reasoning = true;
                if (auto m = r->find("mandatory"); m != r->end() && m->is_boolean())
                    model.reasoning_mandatory = m->get<bool>();
                if (auto d = r->find("default_effort"); d != r->end() && d->is_string())
                    model.reasoning_default_effort = d->get<std::string>();
                if (auto se = r->find("supported_efforts"); se != r->end() && se->is_array()) {
                    for (const auto& level : *se)
                        if (level.is_string())
                            model.reasoning_efforts.push_back(level.get<std::string>());
                }
            } else if (r->is_boolean() && r->get<bool>()) {
                model.supports_reasoning = true;
            }
        }
        caps.push_back(std::move(model));
    }
    std::sort(caps.begin(), caps.end(),
              [](const ModelCapabilities& a, const ModelCapabilities& b) { return a.id < b.id; });
    caps.erase(std::unique(caps.begin(), caps.end(),
                           [](const ModelCapabilities& a, const ModelCapabilities& b) {
                               return a.id == b.id;
                           }),
               caps.end());
    return caps;
}
} // namespace mini_agent
