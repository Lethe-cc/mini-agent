#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/diff.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace mini_agent {
namespace {

// Shared helper for FileRuntime: string fields with fail-closed errors.
bool get_string(const nlohmann::json& args, const char* key, std::string* out,
                ToolResult* failure) {
    auto it = args.find(key);
    if (it == args.end() || !it->is_string()) {
        *failure = ToolResult{std::string("missing string field '") + key + "'", true};
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

// Applies the edit_file replacement semantics to `old_text`: returns the
// rebuilt text, or an error message. Shared by preview() and call() so the
// diff shown at approval matches what executes (design 009 5.3).
std::string apply_edit(const std::string& old_text, const std::string& old_str,
                       const std::string& new_str, bool replace_all, std::string* error) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = old_text.find(old_str, pos)) != std::string::npos) {
        ++count;
        pos += old_str.size();
    }
    if (count == 0) {
        *error = "old_str not found (0 matches); check exact whitespace/content with read_file";
        return {};
    }
    if (count > 1 && !replace_all) {
        *error = "old_str matches " + std::to_string(count) +
                 " places; add context to make it unique or set replace_all";
        return {};
    }
    std::string remainder = old_text;
    std::string rebuilt;
    while ((pos = remainder.find(old_str)) != std::string::npos) {
        rebuilt.append(remainder, 0, pos);
        rebuilt += new_str;
        remainder.erase(0, pos + old_str.size());
    }
    rebuilt += remainder;
    return rebuilt;
}

} // namespace

FileRuntime::FileRuntime(FileToolsConfig config) : config_(std::move(config)) {
}

std::vector<ToolDefinition> FileRuntime::list() {
    std::vector<ToolDefinition> out;
    auto add = [&out](const char* name, const char* description, const char* schema) {
        ToolDefinition tool;
        tool.name = name;
        tool.description = description;
        tool.input_schema_json = schema;
        tool.source = "builtin";
        out.push_back(tool);
    };
    add("read_file",
        "Read a text file inside the workspace, returned as line-numbered text. "
        "Arguments: {\"path\": \"relative or absolute path\", "
        "\"from_line\": optional first line (1-based), \"to_line\": optional last line}.",
        R"({"type":"object","properties":{"path":{"type":"string"},"from_line":{"type":"integer"},"to_line":{"type":"integer"}},"required":["path"]})");
    add("write_file",
        "Create or fully overwrite a text file inside the workspace (parent dir must "
        "exist). Prefer edit_file for changes to existing files. Arguments: "
        "{\"path\": \"...\", \"content\": \"full new content\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})");
    add("edit_file",
        "Replace an exact string inside a file in the workspace. old_str must match "
        "exactly one place unless replace_all is true (include enough context to be "
        "unique; leading whitespace matters). Arguments: {\"path\": \"...\", "
        "\"old_str\": \"...\", \"new_str\": \"...\", \"replace_all\": optional true}.",
        R"({"type":"object","properties":{"path":{"type":"string"},"old_str":{"type":"string"},"new_str":{"type":"string"},"replace_all":{"type":"boolean"}},"required":["path","old_str","new_str"]})");
    add("list_dir",
        "List a directory inside the workspace (non-recursive). Arguments: "
        "{\"path\": \"optional, default workspace root\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"}}})");
    add("search_text",
        "Search text files in the workspace for a substring or regex, line-based. "
        "Returns \"path:line: text\" rows. Arguments: {\"pattern\": \"...\", "
        "\"is_regex\": optional false, \"path_prefix\": optional subdir/file filter}.",
        R"({"type":"object","properties":{"pattern":{"type":"string"},"is_regex":{"type":"boolean"},"path_prefix":{"type":"string"}},"required":["pattern"]})");
    // v5 (design 011): human-facing file preview. Unlike read_file (which
    // feeds the MODEL line-numbered text), preview_file returns the RAW
    // body so the TUI/CLI can render it for the user (Markdown files are
    // rendered, others shown verbatim). Its result is surfaced through the
    // result_preview() hook, not the model turn.
    add("preview_file",
        "Preview a workspace text file for the human user (the terminal renders it; "
        "Markdown shows formatted). Returns the raw file body, not line numbers. Use "
        "this to SHOW a file to the user; use read_file to inspect content yourself. "
        "Arguments: {\"path\": \"relative or absolute path\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})");
    return out;
}

namespace {

// Reads a whole file with size caps; returns nullopt + ToolResult on any
// failure. Binary probe: NUL byte in the first 8 KiB.
bool read_file_bytes(const std::filesystem::path& path, std::size_t limit, std::string* content,
                     ToolResult* failure) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        *failure = ToolResult{"cannot stat: " + ec.message(), true};
        return false;
    }
    if (size > limit) {
        *failure = ToolResult{"file too large (" + std::to_string(size) + " bytes > " +
                                  std::to_string(limit) +
                                  "); read a line range or use search_text/run_bash head",
                              true};
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *failure = ToolResult{"cannot open file", true};
        return false;
    }
    content->assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return true;
}

std::string relative_to_root(const std::filesystem::path& root,
                             const std::filesystem::path& target) {
    auto root_it = root.begin();
    auto target_it = target.begin();
    while (root_it != root.end() && target_it != target.end() && *root_it == *target_it) {
        ++root_it;
        ++target_it;
    }
    std::filesystem::path rel;
    for (auto it = target_it; it != target.end(); ++it) {
        if (it != target_it)
            rel /= *it;
        else
            rel = *it;
    }
    return rel.empty() ? std::string(".") : rel.string();
}

} // namespace

std::string FileRuntime::preview(const ToolCall& call) {
    if (call.name != "write_file" && call.name != "edit_file")
        return {};
    auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object())
        return {};
    std::string path_raw;
    if (!args.contains("path") || !args["path"].is_string())
        return {};
    path_raw = args["path"].get<std::string>();
    std::filesystem::path resolved;
    std::string bounds_error;
    if (!resolve_within_workspace(config_.workspace, path_raw, &resolved, &bounds_error))
        return {};
    std::error_code ec;
    std::string old_text;
    const bool exists = std::filesystem::is_regular_file(resolved, ec);
    if (exists) {
        std::ifstream stream(resolved, std::ios::binary);
        if (!stream)
            return {};
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        old_text = buffer.str();
    }
    std::string new_text;
    if (call.name == "write_file") {
        if (!args.contains("content") || !args["content"].is_string())
            return {};
        new_text = args["content"].get<std::string>();
    } else { // edit_file
        if (!args.contains("old_str") || !args.contains("new_str") ||
            !args["old_str"].is_string() || !args["new_str"].is_string())
            return {};
        const std::string old_str = args["old_str"].get<std::string>();
        if (old_str.empty())
            return {};
        std::string edit_error;
        new_text = apply_edit(old_text, old_str, args["new_str"].get<std::string>(),
                              args.value("replace_all", false), &edit_error);
        if (!edit_error.empty())
            return "edit_file: " + edit_error; // diff would be invalid; say so at approval
    }
    const auto result = exists ? diff_texts(old_text, new_text) : diff_against_empty(new_text);
    // Arm the race guard with the exact OLD content we previewed.
    armed_previews_[resolved.string()] = old_text;
    const auto label = relative_to_root(config_.workspace, resolved);
    auto text = result.to_unified_text("a/" + label, "b/" + label);
    if (text.empty())
        return "(no content change)";
    return text;
}

void FileRuntime::disarm(const ToolCall& call) {
    if (call.name != "write_file" && call.name != "edit_file")
        return;
    auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object() || !args.contains("path") ||
        !args["path"].is_string())
        return;
    std::filesystem::path resolved;
    std::string error;
    if (!resolve_within_workspace(config_.workspace, args["path"].get<std::string>(), &resolved,
                                  &error))
        return;
    armed_previews_.erase(resolved.string());
}

// v5 (design 011): post-execution transcript preview. write_file/edit_file
// -> the APPLIED unified diff staged by call(); preview_file -> the raw
// body staged for the UI viewer. Consumed once (staged state cleared) so a
// later unrelated tool call cannot resend stale content.
std::string FileRuntime::result_preview(const ToolCall& call, const ToolResult& result) {
    if (result.is_error)
        return {};
    if (call.name == "write_file" || call.name == "edit_file") {
        std::string diff;
        diff.swap(last_applied_diff_);
        last_applied_path_.clear();
        if (diff.empty())
            return {};
        return diff;
    }
    if (call.name == "preview_file") {
        std::string body;
        body.swap(last_preview_body_);
        last_preview_path_.clear();
        return body;
    }
    return {};
}

ToolResult FileRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    namespace fs = std::filesystem;
    ToolResult bad{"arguments must be a JSON object", true};
    auto parsed = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return bad;
    auto resolve = [&](const std::string& raw, fs::path* out) -> std::optional<ToolResult> {
        std::string error;
        if (!resolve_within_workspace(config_.workspace, raw, out, &error))
            return ToolResult{raw + ": " + error, true};
        return std::nullopt;
    };
    auto abort_check = [&]() -> std::optional<ToolResult> {
        if (should_abort())
            return ToolResult{"aborted by user", true};
        return std::nullopt;
    };

    if (call.name == "read_file") {
        std::string path_raw;
        if (!get_string(parsed, "path", &path_raw, &bad))
            return bad;
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec))
            return ToolResult{path_raw + ": not a regular file", true};
        std::string content;
        if (!read_file_bytes(resolved, config_.read_limit_bytes, &content, &bad))
            return bad;
        if (content.find('\0') != std::string::npos)
            return ToolResult{path_raw + ": binary file (NUL found); use run_bash", true};
        // Line-range slice with cat -n numbering.
        const auto lines = diff_split_lines(content);
        std::size_t from = 1;
        std::size_t to = lines.size();
        if (parsed.contains("from_line") && parsed["from_line"].is_number_integer())
            from = static_cast<std::size_t>(
                std::max<std::int64_t>(1, parsed["from_line"].get<int64_t>()));
        if (parsed.contains("to_line") && parsed["to_line"].is_number_integer())
            to = static_cast<std::size_t>(
                std::min(std::max<std::int64_t>(0, parsed["to_line"].get<int64_t>()),
                         static_cast<std::int64_t>(lines.size())));
        if (lines.empty())
            return ToolResult{"(empty file)", false};
        if (from > to || from > lines.size())
            return ToolResult{"empty line range", false};
        std::ostringstream out;
        for (std::size_t i = from; i <= to; ++i) {
            if (auto early = abort_check())
                return *early;
            out << i << '\t' << lines[i - 1] << '\n';
        }
        std::string shown = out.str();
        if (shown.size() > config_.read_limit_bytes)
            shown = cap_tool_output(shown, config_.read_limit_bytes);
        return ToolResult{shown + "\n(" + path_raw + ": " + std::to_string(to - from + 1) + " of " +
                              std::to_string(lines.size()) + " lines)",
                          false};
    }

    if (call.name == "preview_file") {
        // v5 (design 011): raw body for the human viewer (the UI renders
        // it; the model gets only a short confirmation back). Same
        // boundary, size cap, and binary posture as read_file.
        std::string path_raw;
        if (!get_string(parsed, "path", &path_raw, &bad))
            return bad;
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec))
            return ToolResult{path_raw + ": not a regular file", true};
        std::string content;
        if (!read_file_bytes(resolved, config_.read_limit_bytes, &content, &bad))
            return bad;
        if (content.find('\0') != std::string::npos)
            return ToolResult{path_raw + ": binary file (NUL found); use run_bash", true};
        if (content.empty())
            return ToolResult{"(empty file)", false};
        // The body is carried to the UI via result_preview(); the model
        // only needs to know it was shown (keeps tokens out of the chat).
        const std::size_t body_bytes = content.size();
        last_preview_path_ = path_raw;
        last_preview_body_ = std::move(content);
        return ToolResult{"previewed " + path_raw + " (" + std::to_string(body_bytes) +
                              " bytes) for the user",
                          false};
    }

    if (call.name == "write_file" || call.name == "edit_file") {
        std::string path_raw;
        if (!get_string(parsed, "path", &path_raw, &bad))
            return bad;
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        // v5 (design 011): the pre-write body, captured for the applied
        // diff staged after a successful rename. Empty/absent => the file
        // is new and the diff runs against empty.
        std::string pre_image;
        bool has_pre_image = false;
        // Race guard (design 009 5.3): if a preview armed the expected old
        // content, refuse when the file changed underneath us since. Runs
        // BEFORE any content work so the refusal is about the race, not a
        // spurious "not found" from the mutated file.
        auto armed = armed_previews_.find(resolved.string());
        if (armed != armed_previews_.end()) {
            std::string current;
            std::ifstream stream(resolved, std::ios::binary);
            bool readable = static_cast<bool>(stream);
            if (readable) {
                std::ostringstream buffer;
                buffer << stream.rdbuf();
                current = buffer.str();
            }
            const bool gone = !readable && !fs::exists(resolved, ec);
            // Unreadable-but-existing: treat as changed (cannot verify).
            // Deleted after a non-empty preview: also changed (the approved
            // diff's base vanished). Deleted after an empty preview (new
            // file): still the creation the user approved.
            const bool mismatch =
                readable ? (current != armed->second) : (gone ? !armed->second.empty() : true);
            if (mismatch) {
                armed_previews_.erase(armed);
                return ToolResult{
                    path_raw + ": changed since the approved preview; re-read and retry", true};
            }
            // The file is byte-identical to the approved preview: that
            // snapshot IS the pre-image (and it also covers a new file:
            // armed empty + not readable => not the mismatch branch).
            pre_image = armed->second;
            has_pre_image = true;
            armed_previews_.erase(armed);
        }
        std::string new_text;
        if (call.name == "write_file") {
            if (!get_string(parsed, "content", &new_text, &bad))
                return bad;
        } else {
            std::string old_str;
            std::string new_str;
            if (!get_string(parsed, "old_str", &old_str, &bad))
                return bad;
            if (!get_string(parsed, "new_str", &new_str, &bad))
                return bad;
            if (old_str.empty())
                return ToolResult{"edit_file: 'old_str' must be non-empty", true};
            std::string& old_text = pre_image; // doubles as the pre-image
            has_pre_image = true;              // for the applied diff below
            if (!read_file_bytes(resolved, config_.write_limit_bytes, &old_text, &bad))
                return bad;
            std::string edit_error;
            new_text = apply_edit(old_text, old_str, new_str, parsed.value("replace_all", false),
                                  &edit_error);
            if (!edit_error.empty())
                return ToolResult{"edit_file: " + edit_error + " in " + path_raw, true};
        }
        if (new_text.size() > config_.write_limit_bytes)
            return ToolResult{"content too large for the write cap", true};
        if (auto early = abort_check())
            return *early;
        if (!fs::exists(resolved.parent_path(), ec))
            return ToolResult{path_raw + ": parent directory does not exist", true};
        const bool existed = fs::exists(resolved, ec);
        const std::string tmp = resolved.string() + ".mini-agent.tmp";
        {
            // Mode applies only on create; existing files keep theirs via
            // O_TRUNC. New files: 0644 & ~umask (same posture as any editor).
            const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
                return ToolResult{path_raw + ": open failed: " + std::strerror(errno), true};
            const ssize_t written = ::write(fd, new_text.data(), new_text.size());
            const bool ok = written == static_cast<ssize_t>(new_text.size());
            if (ok)
                ::fsync(fd);
            ::close(fd);
            if (!ok) {
                ::unlink(tmp.c_str());
                return ToolResult{path_raw + ": short write", true};
            }
        }
        // Preserve mode for existing files; new files get 0644 & ~umask via
        // open above. rename is atomic within the directory.
        if (::rename(tmp.c_str(), resolved.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return ToolResult{path_raw + ": rename failed: " + std::strerror(errno), true};
        }
        // v5 (design 011): stage the APPLIED diff for result_preview().
        // Pre-image sources, in order: the race-guard snapshot (approval
        // flow), the re-read body (edit_file), else the file as it stands
        // right before this write (write_file without a preview). On any
        // read failure the diff falls back to "new file against empty".
        if (!has_pre_image) {
            if (existed) {
                std::ifstream before(resolved, std::ios::binary);
                if (before) {
                    std::ostringstream buffer;
                    buffer << before.rdbuf();
                    pre_image = buffer.str();
                }
            } else {
                has_pre_image = true; // new file: empty pre-image is final
            }
        }
        {
            const auto result =
                pre_image.empty() ? diff_against_empty(new_text) : diff_texts(pre_image, new_text);
            const auto label = relative_to_root(config_.workspace, resolved);
            last_applied_path_ = path_raw;
            last_applied_diff_ =
                result.changed ? result.to_unified_text("a/" + label, "b/" + label) : std::string();
        }
        return ToolResult{std::string(existed ? "wrote " : "created ") + path_raw + " (" +
                              std::to_string(new_text.size()) + " bytes)",
                          false};
    }

    if (call.name == "list_dir") {
        std::string path_raw = ".";
        if (parsed.contains("path") && parsed["path"].is_string())
            path_raw = parsed["path"].get<std::string>();
        else if (parsed.contains("path"))
            return ToolResult{"'path' must be a string", true};
        fs::path resolved;
        if (auto early = resolve(path_raw, &resolved))
            return *early;
        std::error_code ec;
        if (!fs::is_directory(resolved, ec))
            return ToolResult{path_raw + ": not a directory", true};
        std::vector<std::string> entries;
        for (fs::directory_iterator it(resolved, ec), end; it != end; it.increment(ec)) {
            if (ec)
                break;
            if (auto early = abort_check())
                return *early;
            const auto name = it->path().filename().string();
            std::string suffix;
            if (it->is_directory(ec))
                suffix = "/";
            else if (it->is_symlink(ec))
                suffix = "@";
            entries.push_back(name + suffix);
            if (entries.size() >= config_.list_max_entries)
                break;
        }
        std::sort(entries.begin(), entries.end());
        std::ostringstream out;
        for (const auto& entry : entries)
            out << entry << '\n';
        if (entries.size() >= config_.list_max_entries)
            out << "(listing capped at " << config_.list_max_entries << " entries)\n";
        return ToolResult{out.str().empty() ? std::string("(empty directory)") : out.str(), false};
    }

    if (call.name == "search_text") {
        std::string pattern;
        if (!get_string(parsed, "pattern", &pattern, &bad))
            return bad;
        if (pattern.empty())
            return ToolResult{"'pattern' must be non-empty", true};
        const bool is_regex = parsed.value("is_regex", false);
        std::string prefix_raw = ".";
        if (parsed.contains("path_prefix") && parsed["path_prefix"].is_string())
            prefix_raw = parsed["path_prefix"].get<std::string>();
        fs::path resolved;
        if (auto early = resolve(prefix_raw, &resolved))
            return *early;
        std::ostringstream out;
        std::size_t matches = 0;
        bool cap_hit = false;
        std::regex re;
        if (is_regex) {
            try {
                re.assign(pattern, std::regex::optimize);
            } catch (const std::regex_error& error) {
                return ToolResult{"invalid regex: " + std::string(error.what()), true};
            }
        }
        std::error_code ec;
        if (fs::is_regular_file(resolved, ec)) {
            // Single-file mode: search one file directly.
            std::string content;
            if (!read_file_bytes(resolved, config_.write_limit_bytes, &content, &bad))
                return bad;
            const auto lines = diff_split_lines(content);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const bool hit = is_regex ? std::regex_search(std::string(lines[i]), re)
                                          : lines[i].find(pattern) != std::string::npos;
                if (!hit)
                    continue;
                out << prefix_raw << ':' << (i + 1) << ": " << lines[i] << '\n';
                if (++matches >= config_.search_max_matches) {
                    cap_hit = true;
                    break;
                }
            }
        } else {
            std::error_code iter_ec;
            fs::recursive_directory_iterator it(
                resolved, fs::directory_options::skip_permission_denied, iter_ec);
            if (iter_ec)
                return ToolResult{prefix_raw + ": cannot open directory: " + iter_ec.message(),
                                  true};
            const fs::recursive_directory_iterator end;
            for (; it != end; it.increment(iter_ec)) {
                if (iter_ec)
                    break;
                if (auto early = abort_check())
                    return *early;
                if (it.depth() > 0 && it->path().filename() == ".git") {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code type_ec;
                if (!it->is_regular_file(type_ec) || type_ec)
                    continue;
                const auto rel = relative_to_root(config_.workspace, it->path());
                std::error_code size_ec;
                const auto size = fs::file_size(it->path(), size_ec);
                if (size_ec || size > config_.write_limit_bytes)
                    continue; // skip unreadable/huge without failing the search
                std::string content;
                ToolResult read_failure{"", false};
                if (!read_file_bytes(it->path(), size, &content, &read_failure))
                    continue;
                if (content.find('\0') != std::string::npos)
                    continue; // binary
                const auto lines = diff_split_lines(content);
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    const bool hit = is_regex ? std::regex_search(std::string(lines[i]), re)
                                              : lines[i].find(pattern) != std::string::npos;
                    if (!hit)
                        continue;
                    out << rel << ':' << (i + 1) << ": " << lines[i] << '\n';
                    if (++matches >= config_.search_max_matches) {
                        cap_hit = true;
                        break;
                    }
                }
                if (cap_hit)
                    break;
            }
        }
        if (cap_hit)
            out << "(match cap hit at " << config_.search_max_matches << ")\n";
        auto text = out.str();
        if (text.empty())
            return ToolResult{"(no matches)", false};
        return ToolResult{cap_tool_output(text, BuiltinRuntime::kOutputLimitBytes), false};
    }

    return ToolResult{"unknown file tool: " + call.name, true};
}

// ---------------------------------------------------------------------------
// GitRuntime (design 009 5.2, ADR 008)
// ---------------------------------------------------------------------------

GitRuntime::GitRuntime(std::filesystem::path workspace, std::size_t timeout_seconds)
    : workspace_(std::move(workspace)), timeout_seconds_(timeout_seconds) {
}
} // namespace mini_agent
