#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mini_agent/chat.hpp"

namespace mini_agent {

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

} // namespace mini_agent
