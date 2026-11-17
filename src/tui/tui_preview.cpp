// TUI preview pane helpers (design 019 P4). Bodies moved verbatim from the
// run_loop closures in tui.cpp, parameterized over workspace/colors.
#include "tui_preview.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "mini_agent/highlight.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/tools.hpp"

namespace mini_agent::tui {

bool load_preview_body(const std::string& workspace, const std::string& path_raw, std::string* body,
                       std::string* err, const std::string& root_override) {
    std::filesystem::path resolved;
    std::string bounds_error;
    const std::string root =
        !root_override.empty()
            ? root_override
            : (workspace.empty() ? std::filesystem::current_path().string()
                                 : std::filesystem::weakly_canonical(workspace).string());
    if (!resolve_within_workspace(root, path_raw, &resolved, &bounds_error)) {
        *err = "preview " + sanitize_display(path_raw) + ": " + sanitize_display(bounds_error);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(resolved, ec)) {
        *err = "preview: not a regular file: " + sanitize_display(path_raw);
        return false;
    }
    std::ifstream stream(resolved, std::ios::binary);
    if (!stream) {
        *err = "preview: cannot open: " + sanitize_display(path_raw);
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *body = buffer.str();
    if (body->find('\0') != std::string::npos) {
        *err = "preview: binary file: " + sanitize_display(path_raw);
        return false;
    }
    return true;
}

std::vector<std::string> render_preview_lines(bool colors, const std::string& body,
                                              const std::string& path_raw, std::size_t cols) {
    std::vector<std::string> out;
    std::string lower = path_raw;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool markdown =
        lower.size() >= 3 &&
        (lower.compare(lower.size() - 3, 3, ".md") == 0 ||
         (lower.size() >= 9 && lower.compare(lower.size() - 9, 9, ".markdown") == 0));
    const std::size_t width = cols > 1 ? cols - 1 : 1;
    if (markdown) {
        std::string pline;
        std::istringstream pr(md::render(body, width));
        while (std::getline(pr, pline))
            out.push_back(pline.empty() ? std::string(" ") : pline);
    } else {
        const auto lang = hl::language_for_path(path_raw);
        const bool lit = !lang.empty() && colors;
        std::string pline;
        std::istringstream pr(body);
        hl::State hstate;
        std::size_t shown = 0;
        while (std::getline(pr, pline) && shown < 500) {
            const auto safe = sanitize_display(pline);
            out.push_back(lit ? hl::highlight_line(safe, lang, hstate) : "  " + safe);
            ++shown;
        }
        if (!pr.eof())
            out.push_back("  ... truncated (file longer than 500 lines)");
    }
    return out;
}

} // namespace mini_agent::tui