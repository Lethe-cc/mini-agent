#include "mini_agent/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace mini_agent {
namespace {

// Splits into lines without terminators. `content` points into `text`
// (string_view, no copy). The final segment is dropped when it is empty
// (text ended with '\n'): "a\n" and "a\nb\n" both start ["a", ...].
void push_segments(std::string_view text, std::vector<std::string_view>* out) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out->push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size())
        out->push_back(text.substr(start));
}

// Line-based LCS diff. Builds the diagonal (Myers-free) O(n*m) edit graph
// with uint32 cell values: cell (i,j) = LCS length of suffixes. Table
// memory is the guard against pathological inputs (see DiffOptions cap).
struct EditScript {
    std::vector<char> ops; // 'k' keep, '-' remove, '+' add (interleaved)
};

EditScript lcs_script(const std::vector<std::string_view>& a,
                      const std::vector<std::string_view>& b) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    // (n+1)*(m+1) cells, row-major.
    std::vector<uint32_t> table((n + 1) * (m + 1), 0);
    auto at = [m](std::size_t i, std::size_t j) { return i * (m + 1) + j; };
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            if (a[i] == b[j]) {
                table[at(i, j)] = table[at(i + 1, j + 1)] + 1;
            } else {
                table[at(i, j)] = std::max(table[at(i + 1, j)], table[at(i, j + 1)]);
            }
        }
    }
    EditScript script;
    script.ops.reserve(n + m);
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            script.ops.push_back('k');
            ++i;
            ++j;
        } else if (table[at(i + 1, j)] >= table[at(i, j + 1)]) {
            script.ops.push_back('-');
            ++i;
        } else {
            script.ops.push_back('+');
            ++j;
        }
    }
    while (i < n) {
        script.ops.push_back('-');
        ++i;
    }
    while (j < m) {
        script.ops.push_back('+');
        ++j;
    }
    return script;
}

} // namespace

std::vector<std::string_view> diff_split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    push_segments(text, &out);
    return out;
}

std::string DiffResult::to_unified_text(std::string_view old_label,
                                        std::string_view new_label) const {
    if (!changed)
        return {};
    std::string out;
    out += "--- ";
    out += old_label;
    out += "\n+++ ";
    out += new_label;
    out += '\n';
    for (const auto& row : rows) {
        if (row.kind != '@')
            out += row.kind; // '@' rows carry their own "@@ ... @@" marker
        out += row.text;
        out += '\n';
    }
    if (!note.empty()) {
        out += ": ";
        out += note;
        out += '\n';
    }
    return out;
}

DiffResult diff_texts(std::string_view old_text, std::string_view new_text,
                      const DiffOptions& options) {
    DiffResult result;
    const auto a = diff_split_lines(old_text);
    const auto b = diff_split_lines(new_text);
    if (a == b)
        return result; // unchanged: no rows, caller shows "new file" note
    result.changed = true;

    if (a.size() > options.max_diff_lines || b.size() > options.max_diff_lines) {
        result.too_large = true;
        result.note = "file too large for diff preview (" + std::to_string(a.size()) + " -> " +
                      std::to_string(b.size()) + " lines)";
        return result;
    }
    // Real OOM guard for the O(n*m) table: two mid-sized files can pass the
    // line cap yet demand gigabytes (4 bytes/cell).
    if (a.size() * b.size() > options.max_diff_cells) {
        result.too_large = true;
        result.note = "diff too large to preview (" + std::to_string(a.size()) + " x " +
                      std::to_string(b.size()) + " lines)";
        return result;
    }

    const auto script = lcs_script(a, b);

    // Map the interleaved op list to (ai, bi) coordinates per op.
    struct Item {
        char op;
        std::size_t ai; // valid for k/-
        std::size_t bi; // valid for k/+
    };
    std::vector<Item> items;
    items.reserve(script.ops.size());
    std::size_t ai = 0;
    std::size_t bi = 0;
    for (char op : script.ops) {
        items.push_back(Item{op, ai, bi});
        if (op == 'k') {
            ++ai;
            ++bi;
        } else if (op == '-') {
            ++ai;
        } else {
            ++bi;
        }
    }
    result.added =
        std::count_if(items.begin(), items.end(), [](const Item& it) { return it.op == '+'; });
    result.removed =
        std::count_if(items.begin(), items.end(), [](const Item& it) { return it.op == '-'; });

    // Group into hunks: every changed op plus `context` keeps around it.
    const std::size_t ctx = options.context_lines;
    std::size_t idx = 0;
    std::size_t last_end = 0; // never reopen rows already emitted
    bool truncated = false;
    while (idx < items.size()) {
        if (items[idx].op == 'k') {
            ++idx;
            continue;
        }
        // Expand a changed run backward to include context keeps, clamped
        // so a nearby next change cannot re-emit the previous hunk's tail.
        std::size_t start = idx;
        const std::size_t first_change = idx;
        if (ctx > 0)
            start = (first_change > ctx) ? first_change - ctx : 0;
        start = std::max(start, last_end);
        std::size_t end = idx;
        while (end < items.size()) {
            if (items[end].op != 'k') {
                ++end;
                continue;
            }
            // keep run: merge with the next change when the two hunks'
            // context would touch -- git xdiff's rule (gap <= 2*ctx), so
            // approval blocks read exactly like `git diff` (2026-11-05
            // comparison recorded in 009 section 9).
            std::size_t next_change = end;
            while (next_change < items.size() && items[next_change].op == 'k')
                ++next_change;
            if (next_change < items.size() && next_change - end <= 2 * ctx) {
                end = next_change; // merge hunks
                continue;
            }
            end = std::min(end + ctx, items.size());
            break;
        }
        // Hunk header uses real line coordinates in each file.
        const std::size_t old_start = items[start].ai + 1;
        const std::size_t new_start = items[start].bi + 1;
        std::size_t old_count = 0;
        std::size_t new_count = 0;
        for (std::size_t i = start; i < end; ++i) {
            if (items[i].op != '+')
                ++old_count;
            if (items[i].op != '-')
                ++new_count;
        }
        std::string header = "@@ -" + std::to_string(old_start) + "," + std::to_string(old_count) +
                             " +" + std::to_string(new_start) + "," + std::to_string(new_count) +
                             " @@";
        result.rows.push_back(DiffRow{'@', std::move(header)});
        const std::size_t row_budget = options.max_hunk_rows;
        const std::size_t produced_before = result.rows.size();
        for (std::size_t i = start; i < end; ++i) {
            if (result.rows.size() - produced_before >= row_budget) {
                truncated = true;
                break;
            }
            const auto& item = items[i];
            if (item.op == 'k') {
                result.rows.push_back(DiffRow{' ', std::string(a[item.ai])});
            } else if (item.op == '-') {
                result.rows.push_back(DiffRow{'-', std::string(a[item.ai])});
            } else {
                result.rows.push_back(DiffRow{'+', std::string(b[item.bi])});
            }
        }
        idx = end;
        last_end = end;
    }
    if (truncated) {
        if (!result.note.empty())
            result.note += "; ";
        result.note += "hunks truncated at " + std::to_string(options.max_hunk_rows) +
                       " rows (full change still applies on approval)";
    }
    return result;
}

DiffResult diff_against_empty(std::string_view new_text, const DiffOptions& options) {
    return diff_texts("", new_text, options);
}

} // namespace mini_agent
