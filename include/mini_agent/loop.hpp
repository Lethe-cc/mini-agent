#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "mini_agent/chat.hpp"
#include "mini_agent/provider.hpp"

namespace mini_agent {

// Single-turn entry point. Pure parameter injection, no globals: one agent
// == one (Chat, Provider) pair (multi-agent extension point, design 007
// section 5.5). Appends the user message, streams the assistant reply, and
// appends it on success. On ProviderError the chat is left untouched at the
// previous turn boundary (fail without partial persistence, design 007
// section 5). Returns the assistant text. v1: no tool loop; the tool_calls
// branch reserved in chat.hpp plugs in here in v2.
std::string run_turn(
    Chat& chat, Provider& provider, std::string_view user_input,
    const std::function<void(std::string_view)>& on_delta,
    const ShouldAbort& should_abort = [] { return false; });

} // namespace mini_agent
