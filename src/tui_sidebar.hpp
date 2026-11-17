#pragma once
// Internal TUI sidebar browser scan (design 019 P4): single-level directory
// listing + repo-scoped git "changed:" section. Pure given (root, browse);
// no screen/session state. Moved out of the run_loop worker closure.
#include <string>
#include <vector>

namespace mini_agent::tui {

struct SidebarResult {
    std::vector<std::string> rows; // plain; render tints
    // Parallel to rows: selectable entries carry a path (absolute for the
    // browser list / git changed entries); headers/blanks are empty (skipped
    // by focus navigation).
    std::vector<std::string> paths;
    // The workspace root the scan ran against (preview resolves against it).
    std::string root;
    // Repo toplevel for the git "changed:" entries (stored absolute).
    std::string repo_root;
    // The directory the browser list was scanned from.
    std::string browse;
    std::string error; // workspace unreadable etc.
};

// Scan `browse` (single level: ./ ../ + sorted non-hidden entries) and append
// the repo-scoped git changed: section (paths absolute vs the repo toplevel).
SidebarResult scan_sidebar(const std::string& root, const std::string& browse);

} // namespace mini_agent::tui