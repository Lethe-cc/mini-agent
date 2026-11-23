// Input-history sanitizing + slash-line persistence (design 022). Split
// from tui_render.cpp's pure helpers because append/load touch the disk;
// sanitize_history_line itself stays pure and mirrors the /login argument
// parsing in tui_slash.hpp (012 §5.2): first token naming a configured
// profile targets that profile, anything else is the key for the active
// profile -- and keys must never reach recall history or the file (007
// §5.6 rule 7).
#include "mini_agent/tui.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace mini_agent::tui {

std::optional<std::string> sanitize_history_line(std::string_view line,
                                                 const std::vector<std::string>& provider_names) {
    if (line.empty())
        return std::nullopt;
    if (line.front() != '/')
        return std::string(line);
    // Recalling a command that immediately exits is a footgun, not history.
    if (line == "/exit" || line.rfind("/exit ", 0) == 0)
        return std::nullopt;
    // Same loose prefix match as handle_slash (rfind("/login", 0)): forms
    // like "/loginx <key>" also set a key, so they must be stripped too.
    if (line.rfind("/login", 0) == 0) {
        const auto space = line.find(' ');
        if (space == std::string_view::npos)
            return std::string(line == "/login" ? line : std::string_view("/login"));
        const auto arg = line.substr(space + 1); // mirrors arg_of
        const auto arg_space = arg.find(' ');
        const auto first = arg_space == std::string_view::npos ? arg : arg.substr(0, arg_space);
        if (std::find(provider_names.begin(), provider_names.end(), first) !=
            provider_names.end())
            return "/login " + std::string(first); // profile name kept, key dropped
        return std::string("/login"); // whole arg was the active profile's key
    }
    return std::string(line);
}

std::filesystem::path input_history_path(const std::filesystem::path& session_root) {
    if (session_root.empty())
        return {};
    return session_root.parent_path() / "input_history";
}

namespace {

// Raw '/'-prefixed lines, tolerant per line; no sanitizing (append needs
// the already-sanitized "/login <name>" forms kept verbatim).
std::vector<std::string> read_slash_lines(const std::filesystem::path& file) {
    std::vector<std::string> out;
    if (file.empty())
        return out;
    std::ifstream in(file);
    if (!in)
        return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() != '/')
            continue; // tolerate junk / hand edits line by line
        out.push_back(std::move(line));
    }
    return out;
}

void truncate_to_cap(std::vector<std::string>& lines, std::size_t cap) {
    if (lines.size() > cap)
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(cap));
}

} // namespace

std::vector<std::string> load_slash_history(const std::filesystem::path& file,
                                            const std::vector<std::string>& provider_names,
                                            std::size_t cap) {
    auto out = read_slash_lines(file);
    // Re-sanitize disk content: a hand-edited (or older) file must not
    // smuggle an inline key back into recall.
    std::vector<std::string> clean;
    clean.reserve(out.size());
    for (const auto& line : out)
        if (auto s = sanitize_history_line(line, provider_names))
            clean.push_back(std::move(*s));
    truncate_to_cap(clean, cap);
    return clean;
}

bool append_slash_history(const std::filesystem::path& file, std::string_view line,
                          std::size_t cap) {
    if (file.empty() || line.empty() || line.front() != '/' || cap == 0 ||
        line.find('\n') != std::string_view::npos)
        return false;
    // Raw read (no re-sanitize): persisted lines are already sanitized, and
    // re-running with an empty profile list would degrade "/login <name>".
    auto lines = read_slash_lines(file);
    if (!lines.empty() && lines.back() == line)
        return true; // adjacent dedup: repeat Enter does not spam the file
    lines.emplace_back(line);
    truncate_to_cap(lines, cap);

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec); // best effort
    std::ostringstream payload;
    for (const auto& l : lines)
        payload << l << '\n';
    const std::string text = payload.str();
    // Atomic replace, same pattern as config save (tmp 0600 + fsync + rename).
    const std::string tmp = file.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    const ssize_t written = ::write(fd, text.data(), text.size());
    const bool ok = written == static_cast<ssize_t>(text.size());
    if (ok)
        ::fsync(fd);
    ::close(fd);
    if (!ok || ::chmod(tmp.c_str(), 0600) != 0 || ::rename(tmp.c_str(), file.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false; // persistence is an enhancement, never a critical path
    }
    return true;
}

} // namespace mini_agent::tui
