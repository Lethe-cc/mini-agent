#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini_agent {

// Full v1+v2 role set (design 007 section 5.5): Tool exists now so session
// files written by future tool rounds still parse with older builds.
enum class Role { System, User, Assistant, Tool };

std::string_view role_to_string(Role role) noexcept;
std::optional<Role> role_from_string(std::string_view name) noexcept;

struct Message {
    Role role = Role::User;
    std::string content;
    // Extension point (design 007 section 5.5). v1 never fills these;
    // tool_call_id round-trips when present. A future tool_calls array
    // field will follow the same "omit while empty" rule.
    std::optional<std::string> tool_call_id;

    friend bool operator==(const Message& lhs, const Message& rhs) noexcept;
};

class Chat {
  public:
    void add(Message message);
    const std::vector<Message>& messages() const noexcept {
        return messages_;
    }

  private:
    std::vector<Message> messages_;
};

// JSON bridge stays out of the header so consumers need not pull in nlohmann.
std::string chat_to_json(const Chat& chat);
// Fails closed: malformed input, non-array root, or unknown role values
// return nullopt with *error set (no silent drops, design 007 section 5.5).
std::optional<Chat> chat_from_json(std::string_view text, std::string* error);

} // namespace mini_agent
