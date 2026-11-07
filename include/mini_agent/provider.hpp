#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "mini_agent/chat.hpp"

namespace mini_agent {

enum class ProviderErrorKind { Network, HttpStatus, Protocol, Cancelled };

class ProviderError : public std::runtime_error {
  public:
    ProviderError(ProviderErrorKind kind, const std::string& message, int http_status = 0)
        : std::runtime_error(message), kind_(kind), http_status_(http_status) {
    }
    ProviderErrorKind kind() const noexcept {
        return kind_;
    }
    // HTTP status for HttpStatus errors; 0 when no response was received.
    // Lets callers tell "the server refused this key" (401/403) apart from
    // "never reached the server" -- needed to report a login/auth state.
    int http_status() const noexcept {
        return http_status_;
    }

  private:
    ProviderErrorKind kind_;
    int http_status_ = 0;
};

// Minimal config: no global singletons anywhere (multi-agent extension
// point, design 007 section 5.5). One provider == one agent endpoint.
struct ProviderConfig {
    std::string base_url; // e.g. https://api.openai.com/v1
    std::string api_key;  // never logged or echoed
    std::string model;
    std::size_t connect_timeout_seconds = 5;
    std::size_t total_timeout_seconds = 120;
    // v4 (design 010): empty = field omitted from the request body, i.e.
    // byte-identical v1..v3 request (regression hook). When set it is
    // passed through verbatim as `reasoning_effort` -- the server is the
    // arbiter of accepted values (config is the authoritative source).
    std::string reasoning_effort;
};

// Abort hook: called from the streaming callback; return true to stop the
// transfer (used by TUI Ctrl-C).
using ShouldAbort = std::function<bool()>;

// ToolCall itself lives in chat.hpp (storage shape == wire shape here;
// arguments_json is raw model output, never parsed at this layer).

// Token usage reported by the provider for one completion (v4, design
// 010). All zero when the gateway sent no usage chunk (some servers
// ignore stream_options.include_usage; then callers fall back to
// estimate_context_tokens). prompt_tokens counts the FULL outgoing context
// (system + history + tools), so it doubles as the context-usage reading.
struct TokenUsage {
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long total_tokens = 0;
    // v4.2: reasoning tokens from completion_tokens_details (when the
    // gateway reports them): >0 proves the reply actually reasoned.
    long long reasoning_tokens = 0;

    bool empty() const noexcept {
        return prompt_tokens == 0 && completion_tokens == 0 && total_tokens == 0;
    }
};

// v4.2 (design 010 §11): the reasoning level as OBSERVED in response
// messages -- distinct from the configured intent. Sources, in trust
// order: gateway echo of `reasoning_effort` in the chunk payload,
// usage.reasoning_tokens > 0, streamed reasoning content deltas.
struct ActualReasoning {
    bool seen = false;                 // any reasoning evidence at all
    std::string effort;                // echoed level string, empty when not echoed
    bool has_reasoning_tokens = false; // usage proved reasoning output
};

// v4.2 (design 010 §11): per-model capability metadata harvested from
// GET /models when the gateway exposes it (OpenRouter does; standard
// OpenAI does not -- absent fields stay empty and callers fall back to
// config). Purely advisory: config remains the authoritative source.
struct ModelCapabilities {
    std::string id;
    long long context_length = 0;               // 0 = gateway did not say
    std::vector<std::string> reasoning_efforts; // supported levels, wire order
    std::string reasoning_default_effort;       // empty = unknown
    bool reasoning_mandatory = false;           // model cannot stop thinking
    bool supports_reasoning = false;            // any reasoning metadata seen
};

// Result of one completion: final text plus any tool calls the model
// requested. When tool_calls is non-empty the text is usually empty or a
// lead-in; the loop treats invocations as the authoritative outcome.
struct CompletionResult {
    std::string text;
    std::vector<ToolCall> tool_calls;
    TokenUsage usage;          // v4: last-seen usage chunk for this round trip
    ActualReasoning reasoning; // v4.2: evidence seen in this response

    bool has_tool_calls() const noexcept {
        return !tool_calls.empty();
    }
};

class Provider {
  public:
    virtual ~Provider() = default;
    // Streams one completion for `chat`. on_delta receives content fragments;
    // returns the full assistant text. Throws ProviderError on failure.
    virtual std::string complete(const Chat& chat,
                                 const std::function<void(std::string_view)>& on_delta,
                                 const ShouldAbort& should_abort) = 0;
    // v2 capability: tool-aware completion. `tools_json` is the OpenAI wire
    // shape ("[]" or empty = no tools, byte-identical v1 request). The base
    // implementation delegates to complete() and returns pure text, so v1
    // providers and test fakes need no changes (design 008 section 5.2).
    virtual CompletionResult complete_chat(const Chat& chat, const std::string& tools_json,
                                           const std::function<void(std::string_view)>& on_delta,
                                           const ShouldAbort& should_abort);
    // Optional capability: list available model ids, sorted
    // (OpenAI-compatible GET {base_url}/models). The base implementation
    // throws ProviderError(Protocol); providers that speak the endpoint
    // override it. The api_key is used only in the Authorization header and
    // never appears in the returned ids or in any ProviderError text.
    virtual std::vector<std::string> list_models();
    // v4.2 capability listing: same endpoint as list_models but retains
    // context_length/reasoning metadata. Base implementation throws
    // ProviderError(Protocol) like list_models; OpenAiProvider overrides.
    virtual std::vector<ModelCapabilities> list_model_capabilities();
};

// Test-visible wrapper around the internal request builder (008 section 7
// byte-identical no-tools regression). Not part of the Provider interface.
// v4 optional params default to the pre-v4 shape, so the existing
// regression call sites stay byte-identical.
std::string preview_chat_request_body(const Chat& chat, const std::string& model,
                                      const std::string& tools_json,
                                      const std::string& reasoning_effort = {},
                                      bool include_usage = false);

// v4 (design 010): character-class token estimate, the no-usage fallback
// for the context reading. ASCII words ~4 chars/token, CJK codepoints
// ~1.5 chars each, other scripts ~3; deliberately coarse -- the UI marks
// estimated values with a `~` prefix. Pure function, test-visible.
long long estimate_context_tokens(std::string_view text);

// Parses an OpenAI-compatible GET /models body into model ids (sorted,
// deduplicated). Fail-closed (design 007 section 5.5): malformed JSON, a
// missing/non-array "data", or a non-object root set *error and return
// nullopt; array entries without a string "id" are skipped.
std::optional<std::vector<std::string>> parse_models_json(std::string_view body,
                                                          std::string* error);

// v4.2 (design 010 §11): same GET /models body, but keeps the capability
// metadata a plain id list would drop: per-model context_length and the
// reasoning block (supported_efforts / default_effort / mandatory).
// Fail-closed like parse_models_json (malformed root -> nullopt); entries
// without metadata simply yield empty fields (advisory data, never an
// error). Only used when the gateway exposes the fields (OpenRouter yes,
// standard OpenAI no).
std::optional<std::vector<ModelCapabilities>> parse_models_meta_json(std::string_view body,
                                                                     std::string* error);

// OpenAI-compatible Chat Completions (POST {base_url}/chat/completions,
// stream=true, SSE). Protocol shape is a MUST-VERIFY fact against the
// chosen provider's current docs before relying on it (design 007 section 3).
class OpenAiProvider final : public Provider {
  public:
    explicit OpenAiProvider(ProviderConfig config) : config_(std::move(config)) {
    }
    std::string complete(const Chat& chat, const std::function<void(std::string_view)>& on_delta,
                         const ShouldAbort& should_abort) override;
    // Tool-aware path: request body carries the `tools` array when
    // tools_json is non-empty, and the SSE parser accumulates
    // delta.tool_calls fragments by index (design 008 section 5.2, R2:
    // gateway divergences are a must-verify fact per provider).
    CompletionResult complete_chat(const Chat& chat, const std::string& tools_json,
                                   const std::function<void(std::string_view)>& on_delta,
                                   const ShouldAbort& should_abort) override;
    // OpenAI-compatible GET {base_url}/models. Throws ProviderError on
    // network/HTTP/protocol failure; never echoes the api_key.
    std::vector<std::string> list_models() override;
    // v4.2: same endpoint, capability-preserving parse (design 010 §11).
    std::vector<ModelCapabilities> list_model_capabilities() override;

  private:
    ProviderConfig config_;
};

} // namespace mini_agent
