#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mini_agent/chat.hpp"

namespace mini_agent {

// Session schema levels: 1 = v1 (no tool data); 2 = v2, required as soon as
// any message carries tool_calls/tool_call_id (design 008 section 4). Save
// auto-bumps to this level for tool data; load accepts up to this level.
constexpr int kToolSchemaVersion = 2;

// v1 schema is version 1. Load rules (design 007 section 5.5): higher
// schema_version is rejected; unknown optional fields are ignored;
// unknown roles are rejected by the chat parser.
struct SessionMeta {
    int schema_version = 1;
    std::string id;
    std::string created_at; // ISO-8601 UTC text; opaque to us
    std::string updated_at;
    std::string model;
    std::optional<std::string> parent_id; // forward-read: ignored if unknown
    // v3 (design 009 section 5.4): workspace boundary this session opened
    // with; absent = use the launching config/--cwd value. Optional field:
    // old files parse unchanged.
    std::optional<std::string> workspace;
};

struct Session {
    SessionMeta meta;
    Chat chat;
};

// Default root: $XDG_DATA_HOME/mini-agent/sessions, else
// $HOME/.local/share/mini-agent/sessions. Tests inject an explicit root.
std::filesystem::path default_session_root();

// 8 hex chars of randomness; ids carry no semantics (design 007 section 5.5).
std::string generate_session_id();

// Rejects ids with path separators or dot-leading input (directory
// traversal guard, design 007 section 6).
bool session_id_is_safe(std::string_view id) noexcept;

class SessionStore {
  public:
    static const std::size_t kMaxFileBytes = 10u * 1024u * 1024u;

    explicit SessionStore(std::filesystem::path root) : root_(std::move(root)) {
    }

    // Ensures root exists with 0700. Returns false and sets *error otherwise.
    bool ensure_root(std::string* error);

    // Atomic save: serialize, write <id>.json.tmp.<rand>, fsync-style rename.
    // File mode 0600. Updates meta.updated_at before writing.
    bool save(Session session, std::string* error);

    std::optional<Session> load(const std::string& id, std::string* error);

    // Lists session ids (file stems), sorted. No error on empty dir.
    std::vector<std::string> list(std::string* error);

    bool remove(const std::string& id, std::string* error);

    // Simple lock: <id>.lock held via O_EXCL create with pid inside.
    // Stale locks (dead pid) are reclaimed. Returns false if held elsewhere.
    bool try_lock(const std::string& id, std::string* error);
    void unlock(const std::string& id) noexcept;

  private:
    std::filesystem::path path_for(const std::string& id) const;
    std::filesystem::path lock_path_for(const std::string& id) const;
    std::filesystem::path root_;
};

std::string session_to_json(const SessionMeta& meta, const Chat& chat);
std::optional<Session> session_from_json(std::string_view text, std::string* error);

// v3 default system prompt (design 009 section 5.5). Static identity +
// tool etiquette + the live workspace line. `config.system_prompt`:
// nullopt = this default, "" = disabled (returns empty). Called only when
// creating a fresh session (empty chat).
std::string build_system_prompt(const std::string& workspace, const std::string& model,
                                const std::optional<std::string>& override_prompt);

// Injects the default system prompt as the first message of an empty
// chat (no-op when the chat already has messages or the prompt disabled).
// Returns true when a message was added.
bool inject_system_prompt(Chat& chat, const std::string& workspace, const std::string& model,
                          const std::optional<std::string>& override_prompt);

} // namespace mini_agent
