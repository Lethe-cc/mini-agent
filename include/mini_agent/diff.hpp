#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mini_agent {

// v3 line-level unified diff (design 009 section 5.3). Pure functions,
// no I/O: the callers (FileRuntime preview, TUI approval block) feed
// whole-file texts and render the result. Zero new dependencies: classic
// O(old*new) LCS table -- input size is bounded first (see kMaxDiffLines),
// so the quadratic DP stays within a few tens of MB worst-case.

struct DiffOptions {
    std::size_t max_diff_lines = 20000;   // per side; beyond it: give_up note
    std::size_t max_diff_cells = 4000000; // LCS table cells (uint32, ~16 MB);
                                          // n*m beyond => give_up (real OOM
                                          // guard; the line cap only rejects
                                          // obviously huge single sides)
    std::size_t context_lines = 3;        // hunk context
    std::size_t max_hunk_rows = 200;      // output rows per hunk before truncation
};

// One unified-diff output row (no trailing newline). Kind: '-' removed,
// '+' added, ' ' context, '@' hunk header, ':' informational note.
struct DiffRow {
    char kind = ' ';
    std::string text;
};

struct DiffResult {
    bool changed = false;
    bool too_large = false; // inputs beyond max_diff_lines: no real diff
    std::string note;       // informational (too_large, truncation marks)
    std::vector<DiffRow> rows;
    std::size_t added = 0;
    std::size_t removed = 0;

    // Standard unified-diff text ("--- label / +++ label" header plus
    // rows). Empty when unchanged.
    std::string to_unified_text(std::string_view old_label, std::string_view new_label) const;
};

// Splits text into display lines: keeps content, drops the line
// terminators; a trailing newline does not create an empty last line
// (files that differ only in a final \n still show a one-line change).
std::vector<std::string_view> diff_split_lines(std::string_view text);

// Computes the diff between two whole-file texts.
DiffResult diff_texts(std::string_view old_text, std::string_view new_text,
                      const DiffOptions& options = {});

// Convenience: diff a file against empty content (new-file preview).
DiffResult diff_against_empty(std::string_view new_text, const DiffOptions& options = {});

} // namespace mini_agent
