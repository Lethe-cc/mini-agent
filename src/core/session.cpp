#include "mini_agent/session.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <utility>

#include <nlohmann/json.hpp>

namespace mini_agent {
namespace {

std::string iso8601_utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    char buffer[32];
    std::tm utc{};
    ::gmtime_r(&t, &utc);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

// output/error sink pair; swapped only within this file's reviewed call sites
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool read_capped(const std::filesystem::path& path, std::string* out, std::string* error) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error)
            *error = "cannot open " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *out = buffer.str();
    if (out->size() > SessionStore::kMaxFileBytes) {
        if (error)
            *error = "session file exceeds 10 MiB limit; run session rm if corrupt";
        out->clear();
        return false;
    }
    return true;
}

int pid_alive(const std::string& text) {
    std::istringstream stream(text);
    long pid = 0;
    if (!(stream >> pid) || pid <= 0)
        return -1;
    if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM)
        return 1;
    return 0;
}

} // namespace

std::filesystem::path default_session_root() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "mini-agent" / "sessions";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "mini-agent" / "sessions";
    }
    return {};
}

std::string generate_session_id() {
    static std::mt19937_64 engine(
        std::random_device{}() ^ static_cast<unsigned long>(::getpid()) ^
        static_cast<unsigned long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<unsigned long> dist;
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016lx", dist(engine));
    return buffer;
}

std::vector<std::string> session_prefix_matches(std::string_view input,
                                                const std::vector<std::string>& ids) {
    std::vector<std::string> hits;
    if (input.empty())
        return hits;
    for (const auto& id : ids) {
        if (id == input) {
            // Exact match wins outright: no fan-out to prefix siblings.
            hits.clear();
            hits.push_back(id);
            return hits;
        }
        if (id.size() >= input.size() && id.compare(0, input.size(), input) == 0)
            hits.push_back(id);
    }
    return hits;
}

SessionRefMatch match_session_ref(std::string_view ref, const std::vector<std::string>& ids) {
    SessionRefMatch out;
    auto hits = session_prefix_matches(ref, ids);
    if (hits.size() == 1) { // unique prefix (or exact id, which fans to one)
        out.exact = true;
        out.id = std::move(hits.front());
        return out;
    }
    out.candidates = std::move(hits); // 0 or >1: caller distinguishes empty
    return out;
}

bool session_id_is_safe(std::string_view id) noexcept {
    if (id.empty() || id.size() > 32)
        return false;
    if (id.front() == '.')
        return false;
    for (char c : id) {
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

std::filesystem::path SessionStore::path_for(const std::string& id) const {
    return root_ / (id + ".json");
}

std::filesystem::path SessionStore::lock_path_for(const std::string& id) const {
    return root_ / (id + ".json.lock");
}

std::filesystem::path SessionStore::backup_path_for(const std::string& id) const {
    return root_ / (id + ".pre-compact.json");
}

bool SessionStore::ensure_root(std::string* error) {
    if (root_.empty()) {
        if (error)
            *error = "no session root: set XDG_DATA_HOME or HOME";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) {
        if (error)
            *error = "cannot create " + root_.string() + ": " + ec.message();
        return false;
    }
    if (::chmod(root_.c_str(), 0700) != 0) {
        if (error)
            *error = "cannot chmod 0700 " + root_.string();
        return false;
    }
    return true;
}

bool SessionStore::save(Session session, std::string* error) {
    if (!session_id_is_safe(session.meta.id)) {
        if (error)
            *error = "unsafe session id";
        return false;
    }
    session.meta.updated_at = iso8601_utc_now();
    if (session.meta.created_at.empty()) {
        session.meta.created_at = session.meta.updated_at;
    }
    const auto final_path = path_for(session.meta.id);
    // Unique tmp name so concurrent saves to the same id (which the lock
    // should prevent) cannot clobber each other's partial writes.
    const auto tmp_path = final_path.string() + ".tmp." + generate_session_id();
    {
        const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (error)
                *error = "cannot create temp session file: " + std::string(strerror(errno));
            return false;
        }
        const std::string payload = session_to_json(session.meta, session.chat);
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        const bool ok = written == static_cast<ssize_t>(payload.size());
        if (ok && ::fsync(fd) != 0) {
            // best effort
        }
        const int saved_errno = errno;
        ::close(fd);
        if (!ok) {
            ::unlink(tmp_path.c_str());
            if (error)
                *error = "short write on session temp file";
            return false;
        }
        (void)saved_errno;
        if (::chmod(tmp_path.c_str(), 0600) != 0) {
            ::unlink(tmp_path.c_str());
            if (error)
                *error = "cannot chmod 0600 session temp file";
            return false;
        }
    }
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        if (error)
            *error = "atomic rename failed: " + std::string(strerror(errno));
        return false;
    }
    return true;
}

std::optional<Session> SessionStore::load(const std::string& id, std::string* error) {
    if (!session_id_is_safe(id)) {
        if (error)
            *error = "unsafe session id";
        return std::nullopt;
    }
    std::string text;
    if (!read_capped(path_for(id), &text, error)) {
        return std::nullopt;
    }
    auto session = session_from_json(text, error);
    if (session && session->meta.id.empty()) {
        session->meta.id = id;
    }
    return session;
}

namespace {
bool ends_with(const std::string& text, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}
} // namespace

std::vector<std::string> SessionStore::list(std::string* error) {
    std::vector<std::string> ids;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) {
        if (ec) {
            if (error)
                *error = "cannot stat session dir: " + ec.message();
        }
        return ids; // absent dir is simply no sessions
    }
    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (ec) {
            if (error)
                *error = "cannot list session dir: " + ec.message();
            return ids;
        }
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() > 5 && ends_with(name, ".json") &&
            !ends_with(name, ".json.lock") && !ends_with(name, ".pre-compact.json")) {
            ids.push_back(name.substr(0, name.size() - 5));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<std::string> SessionStore::list_by_recency(std::string* error) {
    // Pair each id with its file mtime, then sort newest-first. `save`
    // rewrites the file, so mtime tracks the last turn; ids with an
    // unreadable stamp sort last (deterministic, never dropped).
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> entries;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec))
        return {}; // absent dir is simply no sessions
    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (ec) {
            if (error)
                *error = "cannot list session dir: " + ec.message();
            return {};
        }
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || name.size() <= 5 || !ends_with(name, ".json") ||
            ends_with(name, ".json.lock") || ends_with(name, ".pre-compact.json"))
            continue;
        std::error_code mec;
        const auto mtime = entry.last_write_time(mec);
        if (mec)
            entries.emplace_back(std::filesystem::file_time_type{},
                                 name.substr(0, name.size() - 5)); // unreadable stamp: sort last
        else
            entries.emplace_back(mtime, name.substr(0, name.size() - 5));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first; // newest first
        return a.second < b.second;   // ties: id order
    });
    std::vector<std::string> ids;
    ids.reserve(entries.size());
    for (const auto& e : entries)
        ids.push_back(e.second);
    return ids;
}

bool SessionStore::remove(const std::string& id, std::string* error) {
    if (!session_id_is_safe(id)) {
        if (error)
            *error = "unsafe session id";
        return false;
    }
    std::error_code ec;
    const auto target = path_for(id);
    if (!std::filesystem::exists(target)) {
        if (error)
            *error = "no such session: " + id;
        return false;
    }
    if (!std::filesystem::remove(target, ec) || ec) {
        if (error)
            *error = "cannot remove session " + id + (ec ? ": " + ec.message() : "");
        return false;
    }
    std::error_code lock_ec;
    std::filesystem::remove(lock_path_for(id), lock_ec);
    // 017: the pre-compaction backup belongs to the session; rm/prune
    // take it along (Q1 default: only the latest backup is kept).
    std::error_code backup_ec;
    std::filesystem::remove(backup_path_for(id), backup_ec);
    return true;
}

bool SessionStore::save_backup(const Session& session, std::string* error) {
    // 017 (design 017 §2.2): same atomic-write discipline as save()
    // (tmp + fsync + chmod 0600 + rename), but without touching meta --
    // the backup is a verbatim snapshot of the pre-compaction state.
    if (!session_id_is_safe(session.meta.id)) {
        if (error)
            *error = "unsafe session id";
        return false;
    }
    const auto final_path = backup_path_for(session.meta.id);
    const auto tmp_path = final_path.string() + ".tmp." + generate_session_id();
    {
        const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (error)
                *error = "cannot create backup temp file: " + std::string(strerror(errno));
            return false;
        }
        const std::string payload = session_to_json(session.meta, session.chat);
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        const bool ok = written == static_cast<ssize_t>(payload.size());
        if (ok && ::fsync(fd) != 0) {
            // best effort
        }
        ::close(fd);
        if (!ok) {
            ::unlink(tmp_path.c_str());
            if (error)
                *error = "short write on backup temp file";
            return false;
        }
        if (::chmod(tmp_path.c_str(), 0600) != 0) {
            ::unlink(tmp_path.c_str());
            if (error)
                *error = "cannot chmod 0600 backup temp file";
            return false;
        }
    }
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        if (error)
            *error = "backup atomic rename failed: " + std::string(strerror(errno));
        return false;
    }
    return true;
}

bool SessionStore::try_lock(const std::string& id, std::string* error) {
    if (!session_id_is_safe(id)) {
        if (error)
            *error = "unsafe session id";
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto path = lock_path_for(id);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            const std::string pid = std::to_string(static_cast<long>(::getpid()));
            const ssize_t written = ::write(fd, pid.data(), pid.size());
            (void)written;
            ::close(fd);
            return true;
        }
        if (errno != EEXIST) {
            if (error)
                *error = "cannot create lock: " + std::string(strerror(errno));
            return false;
        }
        // Reclaim stale locks whose pid is dead.
        std::string holder;
        std::string local_error;
        if (read_capped(path, &holder, &local_error)) {
            if (pid_alive(holder) != 1) {
                ::unlink(path.c_str());
                continue;
            }
        }
        if (error) {
            std::string message = "session " + id;
            message += " is already open (lock held by pid ";
            message += holder;
            message += ")";
            *error = std::move(message);
        }
        return false;
    }
    if (error)
        *error = "cannot acquire lock for session " + id;
    return false;
}

void SessionStore::unlock(const std::string& id) noexcept {
    if (!session_id_is_safe(id))
        return;
    ::unlink(lock_path_for(id).c_str());
}

std::string build_system_prompt(const std::string& workspace, const std::string& model,
                                const std::optional<std::string>& override_prompt,
                                const std::string& mcp_surface_hint) {
    if (override_prompt.has_value())
        return *override_prompt; // "" = disabled, text = full replacement
    std::string prompt = "You are mini-agent, a terminal coding assistant running on Linux.\n";
    if (!model.empty())
        prompt += "Model: " + model + "\n";
    prompt += "Workspace: " + (workspace.empty() ? std::string("(unset)") : workspace) +
              "\n"
              "File and git tools are bounded to the workspace; requests outside it are "
              "rejected (re-check the path with list_dir).\n\n"
              "Tool etiquette:\n"
              "- To inspect code prefer read_file (line-numbered) and search_text over shell "
              "pipes.\n"
              "- To SHOW a file to the user (its rendered content, not your private inspection) "
              "use preview_file -- the terminal renders it; Markdown shows formatted. Use "
              "read_file when you need to read the bytes yourself.\n"
              "- To change code prefer edit_file: old_str must match exactly once (include "
              "surrounding context; whitespace matters); use write_file only for new files or "
              "full rewrites.\n"
              "- After edits, verify with the project's build/test commands via run_bash "
              "before declaring success.\n"
              "- git_status/git_diff/git_log are safe; git_add/git_commit create local "
              "commits and may ask for approval; pushing is always a human action -- never "
              "attempt it yourself.\n"
              "- Some tool calls require interactive approval: if a result reports "
              "\"user denied this tool invocation\", do NOT retry the same call; explain "
              "what you wanted and ask the user.\n"
              "- Keep final answers concise; reference files by path.\n";
    // M2a (design 022 §5.1): one pointer line when MCP servers are
    // configured. Capability-neutral (handshake is lazy); the synthetic
    // tools appear in the tool list only after a capable handshake.
    if (!mcp_surface_hint.empty())
        prompt += "- MCP servers configured: " + mcp_surface_hint +
                  ". If a server offers resources or prompts, tools named <server>__mcp_resources "
                  "/ <server>__mcp_prompts appear in your tool list; call them with action=list "
                  "to discover entries.\n";
    return prompt;
}

bool inject_system_prompt(Chat& chat, const std::string& workspace, const std::string& model,
                          const std::optional<std::string>& override_prompt,
                          const std::string& mcp_surface_hint) {
    if (!chat.messages().empty())
        return false; // only fresh sessions get the prompt
    const auto text = build_system_prompt(workspace, model, override_prompt, mcp_surface_hint);
    if (text.empty())
        return false;
    Message message;
    message.role = Role::System;
    message.content = text;
    chat.add(std::move(message));
    return true;
}

} // namespace mini_agent
