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

// One model-requested tool invocation (v2, design 008 section 5.2). The
// storage shape is vendor-neutral ({id, name, arguments}); the Provider
// maps to and from the OpenAI wire shape. `arguments_json` is the raw
// JSON text produced by the model -- never parsed here (fail-closed
// philosophy: the tool server/runtime is the final arbiter).
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;

    friend bool operator==(const ToolCall& lhs, const ToolCall& rhs) noexcept;
};

struct Message {
    Role role = Role::User;
    std::string content;
    // "Omit while empty" rule (design 007 section 5.5, filled in v2):
    // tool_call_id is set on Tool-role replies; tool_calls only on
    // Assistant messages that request invocations.
    std::optional<std::string> tool_call_id;
    std::vector<ToolCall> tool_calls;

    // True when either tool field is non-empty: forces session files to
    // schema_version 2 so v1 builds refuse rather than silently drop them
    // (design 008 section 4: orphan Tool messages would replay wrongly).
    bool has_tool_data() const noexcept {
        return tool_call_id.has_value() || !tool_calls.empty();
    }

    friend bool operator==(const Message& lhs, const Message& rhs) noexcept;
};

class Chat {
  public:
    void add(Message message);
    const std::vector<Message>& messages() const noexcept {
        return messages_;
    }
    // 015 (design 015 §5): wholesale history replacement, used by /compact
    // to swap the transcript for one summary message. No validation here:
    // the caller owns the shape (same contract as add()).
    void replace(std::vector<Message> messages) {
        messages_ = std::move(messages);
    }
    // v3 (design 009 section 5.5): rewrites the `Workspace:` line inside
    // the FIRST System message (the default prompt) when the session's
    // workspace changes via /cwd. No-op when no leading System message or
    // no such line exists. Everything else in the prompt is untouched.
    void update_system_workspace(const std::string& workspace);

  private:
    std::vector<Message> messages_;
};

// JSON bridge stays out of the header so consumers need not pull in nlohmann.
std::string chat_to_json(const Chat& chat);
// Fails closed: malformed input, non-array root, or unknown role values
// return nullopt with *error set (no silent drops, design 007 section 5.5).
std::optional<Chat> chat_from_json(std::string_view text, std::string* error);

// 015 (design 015 §4C): build the summary request for /compact. Keeps the
// leading System message (if any) and adds ONE User message: a fixed
// summarization instruction plus a bounded transcript of the history
// (each message capped at 1500 chars; over 60000 chars total the middle
// is dropped, head and tail kept). Pure + test-visible; the caller sends
// it through a normal completion WITHOUT tools.
Chat build_compact_request(const Chat& history);

} // namespace mini_agent
