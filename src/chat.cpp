#include "mini_agent/chat.hpp"

#include <nlohmann/json.hpp>

namespace mini_agent {

std::string_view role_to_string(Role role) noexcept {
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";

    case Role::Assistant:
        return "assistant";
    case Role::Tool:
        return "tool";
    }
    return "user"; // unreachable; silences compiler warnings without UB
}

std::optional<Role> role_from_string(std::string_view name) noexcept {
    if (name == "system")
        return Role::System;
    if (name == "user")
        return Role::User;
    if (name == "assistant")
        return Role::Assistant;
    if (name == "tool")
        return Role::Tool;
    return std::nullopt;
}

bool operator==(const ToolCall& lhs, const ToolCall& rhs) noexcept {
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.arguments_json == rhs.arguments_json;
}

bool operator==(const Message& lhs, const Message& rhs) noexcept {
    return lhs.role == rhs.role && lhs.content == rhs.content &&
           lhs.tool_call_id == rhs.tool_call_id && lhs.tool_calls == rhs.tool_calls;
}

void Chat::add(Message message) {
    messages_.push_back(std::move(message));
}

std::string chat_to_json(const Chat& chat) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& message : chat.messages()) {
        nlohmann::json entry;
        entry["role"] = std::string(role_to_string(message.role));
        entry["content"] = message.content;
        if (message.tool_call_id) {
            entry["tool_call_id"] = *message.tool_call_id;
        }
        if (!message.tool_calls.empty()) {
            entry["tool_calls"] = nlohmann::json::array();
            for (const auto& call : message.tool_calls) {
                entry["tool_calls"].push_back(
                    {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments_json}});
            }
        }
        arr.push_back(std::move(entry));
    }
    return arr.dump();
}

std::optional<Chat> chat_from_json(std::string_view text, std::string* error) {
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        if (error)
            *error = "invalid JSON in message history";
        return std::nullopt;
    }
    if (!parsed.is_array()) {
        if (error)
            *error = "message history must be a JSON array";
        return std::nullopt;
    }
    Chat chat;
    std::size_t index = 0;
    for (const auto& entry : parsed) {
        ++index;
        if (!entry.is_object()) {
            if (error)
                *error = "message #" + std::to_string(index) + " is not an object";
            return std::nullopt;
        }
        auto role_it = entry.find("role");
        if (role_it == entry.end() || !role_it->is_string()) {
            if (error)
                *error = "message #" + std::to_string(index) + " has no role string";
            return std::nullopt;
        }
        auto role = role_from_string(role_it->get<std::string_view>());
        if (!role) {
            // Fail closed on unknown roles (design 007 section 5.5).
            if (error)
                *error = "message #" + std::to_string(index) + " has unknown role";
            return std::nullopt;
        }
        auto content_it = entry.find("content");
        if (content_it == entry.end() || !content_it->is_string()) {
            if (error)
                *error = "message #" + std::to_string(index) + " has no content string";
            return std::nullopt;
        }
        Message message;
        message.role = *role;
        message.content = content_it->get<std::string>();
        if (auto tid_it = entry.find("tool_call_id"); tid_it != entry.end()) {
            if (!tid_it->is_string()) {
                if (error)
                    *error = "message #" + std::to_string(index) + " tool_call_id is not a string";
                return std::nullopt;
            }
            message.tool_call_id = tid_it->get<std::string>();
        }
        // tool_calls: fail-closed on any malformed entry (design 008 section
        // 5.2). An assistant message with an empty array parses as "no tool
        // calls" and the field is dropped, matching the omit-while-empty rule.
        if (auto calls_it = entry.find("tool_calls"); calls_it != entry.end()) {
            if (!calls_it->is_array()) {
                if (error)
                    *error = "message #" + std::to_string(index) + " tool_calls is not an array";
                return std::nullopt;
            }
            std::size_t call_index = 0;
            for (const auto& call_node : *calls_it) {
                ++call_index;
                if (!call_node.is_object()) {
                    if (error)
                        *error = "message #" + std::to_string(index) + " tool_call #" +
                                 std::to_string(call_index) + " is not an object";
                    return std::nullopt;
                }
                auto str_field = [&](const char* key, std::string* out) -> bool {
                    auto it = call_node.find(key);
                    if (it == call_node.end() || !it->is_string()) {
                        if (error)
                            *error = "message #" + std::to_string(index) + " tool_call #" +
                                     std::to_string(call_index) + " missing string '" + key + "'";
                        return false;
                    }
                    *out = it->get<std::string>();
                    return true;
                };
                ToolCall call;
                if (!str_field("id", &call.id) || !str_field("name", &call.name) ||
                    !str_field("arguments", &call.arguments_json)) {
                    return std::nullopt;
                }
                message.tool_calls.push_back(std::move(call));
            }
        }
        chat.add(std::move(message));
    }
    return chat;
}

} // namespace mini_agent
