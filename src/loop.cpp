#include "mini_agent/loop.hpp"

namespace mini_agent {

std::string run_turn(Chat& chat, Provider& provider, std::string_view user_input,
                     const std::function<void(std::string_view)>& on_delta,
                     const ShouldAbort& should_abort) {
    Message user;
    user.role = Role::User;
    user.content.assign(user_input);

    // Outgoing history = stored history + this user message. The stored
    // chat is only mutated once the assistant reply fully arrived, so a
    // failed turn leaves history at the previous turn boundary (design
    // 007 section 5). A v2 context compressor would choose what
    // `outgoing` holds here without ever touching `chat` (section 5.5).
    Chat outgoing;
    for (const auto& message : chat.messages()) {
        outgoing.add(Message{message});
    }
    outgoing.add(Message{user});

    const std::string assistant_text = provider.complete(outgoing, on_delta, should_abort);

    chat.add(std::move(user));
    Message assistant;
    assistant.role = Role::Assistant;
    assistant.content = assistant_text;
    chat.add(std::move(assistant));
    return assistant_text;
}

} // namespace mini_agent
