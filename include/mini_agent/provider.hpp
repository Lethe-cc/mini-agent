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
    ProviderError(ProviderErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {
    }
    ProviderErrorKind kind() const noexcept {
        return kind_;
    }

  private:
    ProviderErrorKind kind_;
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
};

// OpenAI-compatible Chat Completions (POST {base_url}/chat/completions,
// stream=true, SSE). Protocol shape is a MUST-VERIFY fact against the
// chosen provider's current docs before relying on it (design 007 section 3).
class OpenAiProvider final : public Provider {
  public:
    explicit OpenAiProvider(ProviderConfig config) : config_(std::move(config)) {
    }
    std::string complete(const Chat& chat, const std::function<void(std::string_view)>& on_delta,
                         const ShouldAbort& should_abort) override;

  private:
    ProviderConfig config_;
};

} // namespace mini_agent
