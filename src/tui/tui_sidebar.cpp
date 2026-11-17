// TUI sidebar browser scan (design 019 P4). Body moved verbatim from the
// run_loop worker closure; parameterized over (root, browse).
#include "tui_sidebar.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "mini_agent/tools.hpp"
#include "mini_agent/tui.hpp"

namespace mini_agent::tui {

SidebarResult scan_sidebar(const std::string& root, const std::string& browse) {
    SidebarResult r;
    try {
        std::error_code ec;
        const std::filesystem::path base(browse);
        if (!std::filesystem::is_directory(base, ec)) {
            r.error = "not a directory: " + browse;
        } else {
            std::string toplevel; // repo root, filled by run_git_status
            auto run_git_status = [&]() -> std::vector<std::string> {
                // Alternating [display, path, display, path, ...].
                std::vector<const char*> argv = {"git",
                                                 "-C",
                                                 root.c_str(),
                                                 "--no-pager",
                                                 "--no-optional-locks",
                                                 "status",
                                                 "--porcelain=v1",
                                                 nullptr};
                SubprocessSpec spec;
                spec.exec_path = "git";
                spec.argv = &argv;
                spec.workspace = root;
                spec.timeout_seconds = 5;
                auto out = run_subprocess(spec, [] { return false; });
                // git reports paths relative to the repo TOPLEVEL, not
                // the workspace (workspace may be a subdir like build/).
                // Keep EVERY changed entry (the list is repo-scoped) but
                // store it ABSOLUTE so preview resolves against the repo
                // root instead of the workspace subdir.
                {
                    std::vector<const char*> targv = {
                        "git", "-C", root.c_str(), "rev-parse", "--show-toplevel", nullptr};
                    SubprocessSpec ts;
                    ts.exec_path = "git";
                    ts.argv = &targv;
                    ts.workspace = root;
                    ts.timeout_seconds = 5;
                    toplevel = run_subprocess(ts, [] { return false; }).text;
                    while (!toplevel.empty() &&
                           (toplevel.back() == '\n' || toplevel.back() == '\r'))
                        toplevel.pop_back();
                }
                std::vector<std::string> lines;
                std::istringstream in(out.text);
                std::string line;
                while (std::getline(in, line)) {
                    std::string xy, path;
                    if (line.size() < 4)
                        continue;
                    std::string code = line.substr(0, 2);
                    std::string file = line.substr(3);
                    if (!file.empty() && file.front() == '"')
                        continue; // quoted (non-ASCII) path: skip
                    const auto glyph = sidebar_status_glyph(code);
                    if (glyph.empty())
                        continue;
                    std::string stored = file;
                    if (!toplevel.empty())
                        stored = (std::filesystem::path(toplevel) / file).string();
                    lines.push_back(glyph + " " + file); // readable, repo-relative
                    lines.push_back(stored);             // absolute for resolution
                }
                return lines;
            };
            // rows and paths grow in lockstep (v4.14 selection):
            // paths[i] is workspace-relative for selectable entries,
            // empty for headers/captions (navigation skips those).
            std::vector<std::string> rows;
            std::vector<std::string> paths;
            auto add = [&](std::string text, std::string path) {
                rows.push_back(std::move(text));
                paths.push_back(std::move(path));
            };
            // 018: single-level browser of `browse` (replaces the old
            // 2-level tree). "." and ".." are selectable navigation rows;
            // Enter on a directory enters it, on a file inserts the path.
            // Entries are stored ABSOLUTE; the git changed: section below
            // stays repo-scoped and unchanged.
            std::vector<std::string> names;
            for (const auto& e : std::filesystem::directory_iterator(base, ec)) {
                if (ec)
                    break;
                const std::string name = e.path().filename().string();
                if (name.front() == '.')
                    continue; // hidden; ./ and ../ added explicitly
                names.push_back(name);
            }
            std::sort(names.begin(), names.end());
            add(sanitize_display(base.string()), std::string()); // header
            add("./", base.string());
            add("../", base.parent_path().string());
            for (const auto& name : names) {
                if (rows.size() >= 24) {
                    add("... (listing capped)", std::string());
                    break;
                }
                const auto child = base / name;
                const bool is_dir = std::filesystem::is_directory(child, ec);
                add(is_dir ? name + "/" : name, child.string());
            }
            r.rows = rows;
            r.paths = paths;
            r.root = root;
            r.browse = base.string();
            auto changed_pairs = run_git_status(); // pairs: display, path; fills toplevel
            r.repo_root = toplevel;                // must be read AFTER run_git_status fills it
            if (!changed_pairs.empty()) {
                r.rows.push_back("");
                r.paths.push_back("");
                r.rows.push_back("changed:");
                r.paths.push_back("");
                for (std::size_t i = 0; i + 1 < changed_pairs.size(); i += 2) {
                    if (r.rows.size() >= 26) {
                        r.rows.push_back("  ...");
                        r.paths.push_back("");
                        break;
                    }
                    r.rows.push_back("  " + changed_pairs[i]);
                    r.paths.push_back(changed_pairs[i + 1]);
                }
            }
        }
    } catch (const std::exception& e) {
        r.error = e.what();
    } catch (...) {
        r.error = "sidebar scan failed";
    }
    return r;
}

} // namespace mini_agent::tui
