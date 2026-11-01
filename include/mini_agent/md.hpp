#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mini_agent::md {

// ANSI-aware terminal geometry + minimal Markdown renderer (design 007
// section 5.6; lives in its own module so wrap/width logic is unit-testable
// and shared by the TUI). No external Markdown library: line-based subset
// (maintainer 2026-11-01: "answers may be markdown").

// Display columns of text that may contain CSI sequences (ESC [ ... m):
// escape sequences and combining/ZWJ/variation codepoints count 0;
// CJK/emoji/fullwidth blocks count 2; else 1. Block-level approximation.
std::size_t display_width(std::string_view text) noexcept;

// Split on embedded newlines, then hard-wrap so EVERY returned row's
// display_width <= cols and occupies exactly one terminal row. Active SGR
// styling is carried into continuation rows; each row is reset-terminated.
// Rows never start with the continuation carrying a stale reset.
std::vector<std::string> wrap(std::string_view text, std::size_t cols);

// Collapse to a single row of at most `cols` columns; if truncated, the
// row ends with "...". Styling is preserved and reset-terminated.
std::string clip(std::string text, std::size_t cols);

// Render a Markdown subset to ANSI-styled lines joined with '\n':
//   #..###### headings (bold cyan), **bold**, *italic*, `code` (inverse),
//   ``` fences (dim, indented, markers dropped), -/*/N. lists (bullets
//   aligned), > blockquotes (│ prefix, dim), --- rules, [text](url) -> text.
// `cols` sizes horizontal rules. Unknown constructs pass through unchanged.
// Output is safe to feed to wrap(); it is NOT terminal-capability aware
// (callers decide when to style).
std::string render(std::string_view markdown, std::size_t cols);

} // namespace mini_agent::md
