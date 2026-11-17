// Pure session (de)serialization (design 019 P2): JSON + Markdown export,
// no filesystem/locking. Moved verbatim from session.cpp; decls in session.hpp.
#include "mini_agent/session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mini_agent {
std::string session_to_json(const SessionMeta& meta, const Chat& chat) {
    nlohmann::json messages = nlohmann::json::parse(chat_to_json(chat));
    nlohmann::json root;
    // v2 rule (design 008 section 4): any tool_calls / tool_call_id in the
    // history forces schema_version 2, because a v1 build would drop
    // tool_calls on read and leave orphan Tool messages that replay wrongly.
    int version = meta.schema_version;
    for (const auto& message : chat.messages()) {
        if (message.has_tool_data()) {
            version = std::max(version, kToolSchemaVersion);
            break;
        }
    }
    root["meta"]["schema_version"] = version;
    root["meta"]["id"] = meta.id;
    root["meta"]["created_at"] = meta.created_at;
    root["meta"]["updated_at"] = meta.updated_at;
    root["meta"]["model"] = meta.model;
    if (meta.parent_id) {
        root["meta"]["parent_id"] = *meta.parent_id;
    }
    if (meta.workspace) {
        root["meta"]["workspace"] = *meta.workspace;
    }
    if (meta.provider) {
        root["meta"]["provider"] = *meta.provider;
    }
    root["messages"] = std::move(messages);
    return root.dump(2);
}

std::optional<Session> session_from_json(std::string_view text, std::string* error) {
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "session file is not a JSON object";
        return std::nullopt;
    }
    const auto& meta_node = parsed["meta"];
    if (!meta_node.is_object()) {
        if (error)
            *error = "session has no meta object";
        return std::nullopt;
    }
    auto version_it = meta_node.find("schema_version");
    if (version_it == meta_node.end() || !version_it->is_number_integer()) {
        if (error)
            *error = "session meta.schema_version missing";
        return std::nullopt;
    }
    const auto version = version_it->get<int>();
    if (version > kToolSchemaVersion) {
        // Forward rejection (design 007 section 5.5). v1 builds max out at 1,
        // so a v2 file read by a v1 binary takes this path with a clear
        // "upgrade" hint instead of silently corrupting the tool round-trip.
        if (error)
            *error = "session schema_version " + std::to_string(version) +
                     " is newer than this build supports (max " +
                     std::to_string(kToolSchemaVersion) + "); upgrade mini-agent";
        return std::nullopt;
    }
    Session session;
    session.meta.schema_version = version;
    auto get_str = [&meta_node](const char* key) -> std::string {
        if (auto it = meta_node.find(key); it != meta_node.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return {};
    };
    session.meta.id = get_str("id");
    session.meta.created_at = get_str("created_at");
    session.meta.updated_at = get_str("updated_at");
    session.meta.model = get_str("model");
    if (auto it = meta_node.find("parent_id"); it != meta_node.end() && it->is_string()) {
        session.meta.parent_id = it->get<std::string>();
    }
    if (auto it = meta_node.find("workspace"); it != meta_node.end() && it->is_string()) {
        session.meta.workspace = it->get<std::string>();
    }
    if (auto it = meta_node.find("provider"); it != meta_node.end() && it->is_string()) {
        session.meta.provider = it->get<std::string>();
    }
    const auto& messages_node = parsed["messages"];
    if (!messages_node.is_array()) {
        if (error)
            *error = "session has no messages array";
        return std::nullopt;
    }
    auto chat = chat_from_json(messages_node.dump(), error);
    if (!chat)
        return std::nullopt;
    session.chat = std::move(*chat);
    return session;
}

std::string session_to_markdown(const Session& session) {
    // 016 (design 016 §2): linear transcript, verbatim content. Fences
    // around tool payloads keep raw JSON/arguments out of the prose flow.
    std::ostringstream out;
    const auto& meta = session.meta;
    out << "# session " << meta.id << "\n\n";
    out << "- model: " << (meta.model.empty() ? "(unknown)" : meta.model) << "\n";
    out << "- created: " << meta.created_at << "\n";
    out << "- updated: " << meta.updated_at << "\n";
    if (meta.workspace)
        out << "- workspace: " << *meta.workspace << "\n";
    if (meta.provider)
        out << "- provider: " << *meta.provider << "\n";
    for (const auto& message : session.chat.messages()) {
        switch (message.role) {
        case Role::System:
            out << "\n## System\n\n";
            break;
        case Role::User:
            out << "\n## User\n\n";
            break;
        case Role::Assistant:
            out << "\n## Assistant\n\n";
            break;
        case Role::Tool:
            out << "\n## Tool";
            if (message.tool_call_id)
                out << " (" << *message.tool_call_id << ")";
            out << "\n\n";
            break;
        }
        if (!message.content.empty())
            out << message.content << "\n";
        for (const auto& call : message.tool_calls) {
            out << "\n```json\n"
                << "{\"id\": " << nlohmann::json(call.id).dump()
                << ", \"name\": " << nlohmann::json(call.name).dump()
                << ", \"arguments\": " << nlohmann::json(call.arguments_json).dump() << "}\n"
                << "```\n";
        }
    }
    return out.str();
}

} // namespace mini_agent
