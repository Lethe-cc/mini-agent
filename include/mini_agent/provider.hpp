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
};

// Abort hook: called from the streaming callback; return true to stop the
// transfer (used by TUI Ctrl-C).
using ShouldAbort = std::function<bool()>;

class Provider {
  public:
    virtual ~Provider() = default;
    // Streams one completion for `chat`. on_delta receives content fragments;
    // returns the full assistant text. Throws ProviderError on failure.
    virtual std::string complete(const Chat& chat,
                                 const std::function<void(std::string_view)>& on_delta,
                                 const ShouldAbort& should_abort) = 0;
    // Optional capability: list available model ids, sorted
    // (OpenAI-compatible GET {base_url}/models). The base implementation
    // throws ProviderError(Protocol); providers that speak the endpoint
    // override it. The api_key is used only in the Authorization header and
    // never appears in the returned ids or in any ProviderError text.
    virtual std::vector<std::string> list_models();
};

// Parses an OpenAI-compatible GET /models body into model ids (sorted,
// deduplicated). Fail-closed (design 007 section 5.5): malformed JSON, a
// missing/non-array "data", or a non-object root set *error and return
// nullopt; array entries without a string "id" are skipped.
std::optional<std::vector<std::string>> parse_models_json(std::string_view body,
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
    // OpenAI-compatible GET {base_url}/models. Throws ProviderError on
    // network/HTTP/protocol failure; never echoes the api_key.
    std::vector<std::string> list_models() override;

  private:
    ProviderConfig config_;
};

} // namespace mini_agent
