#pragma once
// Internal TUI preview pane helpers (design 019 P4): pure body loading and
// line rendering, parameterized (no screen/session state). Moved out of the
// run_loop closures in tui.cpp; the pane open/close/mouse stay in tui.cpp.
#include <cstddef>
#include <string>
#include <vector>

namespace mini_agent::tui {

// Resolve `path_raw` within `workspace` (or `root_override` when non-empty),
// reject non-regular/binary files, and read the body. Errors are sanitized
// for display. Returns false and writes *err on any rejection.
bool load_preview_body(const std::string& workspace, const std::string& path_raw, std::string* body,
                       std::string* err, const std::string& root_override);

// Render a preview body to display lines: Markdown via md::render, otherwise
// syntax-highlighted (when `colors`) with a 500-line cap. `cols` sizes wraps.
std::vector<std::string> render_preview_lines(bool colors, const std::string& body,
                                              const std::string& path_raw, std::size_t cols);

} // namespace mini_agent::tui