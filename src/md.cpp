#include "mini_agent/md.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>

#include "mini_agent/highlight.hpp"

namespace mini_agent::md {
namespace {

// Decode one UTF-8 codepoint at i; returns codepoint and writes byte length.
std::uint32_t decode(std::string_view text, std::size_t i, std::size_t* len) {
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    if (c0 < 0x80) {
        *len = 1;
        return c0;
    }
    std::size_t want = 1;
    std::uint32_t cp = 0;
    if ((c0 & 0xE0) == 0xC0) {
        want = 2;
        cp = c0 & 0x1Fu;
    } else if ((c0 & 0xF0) == 0xE0) {
        want = 3;
        cp = c0 & 0x0Fu;
    } else if ((c0 & 0xF8) == 0xF0) {
        want = 4;
        cp = c0 & 0x07u;
    } else {
        *len = 1;
        return 0xFFFD;
    } // stray continuation: invalid
    if (i + want > text.size()) {
        *len = 1;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k < want; ++k) {
        const unsigned char ck = static_cast<unsigned char>(text[i + k]);
        if ((ck & 0xC0) != 0x80) {
            *len = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (ck & 0x3Fu);
    }
    *len = want;
    return cp;
}

bool zero_width(std::uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           cp == 0x00AD;
}

bool wide(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
           (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x2600 && cp <= 0x27BF) ||
           (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0x1F000 && cp <= 0x1F9FF) ||
           (cp >= 0x1FA70 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

} // namespace

std::size_t display_width(std::string_view text) noexcept {
    std::size_t cols = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1B) { // ESC ... terminator (SGR ends with 'm'; accept common finals)
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size()) {
                    const unsigned char d = static_cast<unsigned char>(text[i]);
                    ++i;
                    if (d >= 0x40 && d <= 0x7E)
                        break; // final byte
                }
            }
            continue;
        }
        std::size_t len = 1;
        const std::uint32_t cp = decode(text, i, &len);
        i += len;
        if (cp == 0xFFFD || zero_width(cp))
            continue;
        cols += wide(cp) ? 2 : 1;
    }
    return cols;
}

namespace {

// Token stream that keeps SGR sequences attached to their text so wrap can
// re-emit active styling on continuation rows.
struct Piece {
    std::string sgr;  // escape sequence(s) preceding the text, may be empty
    std::string text; // printable bytes only, no ESC
};

std::vector<Piece> tokenize(std::string_view text) {
    std::vector<Piece> pieces;
    Piece cur;
    std::size_t i = 0;
    while (i < text.size()) {
        if (static_cast<unsigned char>(text[i]) == 0x1B) {
            std::size_t j = i + 1;
            if (j < text.size() && text[j] == '[') {
                ++j;
                while (j < text.size()) {
                    const unsigned char d = static_cast<unsigned char>(text[j]);
                    ++j;
                    if (d >= 0x40 && d <= 0x7E)
                        break;
                }
            }
            if (!cur.text.empty()) {
                pieces.push_back(std::move(cur));
                cur = Piece{};
            }
            cur.sgr.append(text.substr(i, j - i));
            i = j;
            continue;
        }
        std::size_t len = 1;
        decode(text, i, &len); // validate boundary; keep bytes as-is
        if (i + len > text.size())
            len = 1;
        cur.text.append(text.substr(i, len));
        i += len;
    }
    if (!cur.text.empty() || pieces.empty())
        pieces.push_back(std::move(cur));
    return pieces;
}

// Width of one codepoint at position 0 of its slice (bytes pre-decoded).
std::size_t cp_width(std::string_view bytes) {
    std::size_t len = 1;
    const std::uint32_t cp = decode(bytes, 0, &len);
    if (cp == 0xFFFD || zero_width(cp))
        return 0;
    return wide(cp) ? 2 : 1;
}

} // namespace

std::vector<std::string> wrap(std::string_view text, std::size_t cols) {
    std::vector<std::string> rows;
    std::vector<std::string> logical_lines;
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line))
        logical_lines.push_back(line);
    if (logical_lines.empty())
        logical_lines.emplace_back();

    for (const auto& logical : logical_lines) {
        const auto pieces = tokenize(logical);
        std::string row;
        std::string row_sgr; // styling actively emitted on the current row
        std::size_t used = 0;
        auto flush = [&]() {
            if (!row_sgr.empty())
                row += "\x1b[0m";
            rows.push_back(row);
            row.clear();
            row_sgr.clear();
            used = 0;
        };
        for (const auto& piece : pieces) {
            bool emitted_for_row = piece.sgr.empty();
            std::size_t consumed = 0;
            while (consumed < piece.text.size()) {
                std::size_t blen = 1;
                decode(piece.text, consumed, &blen);
                if (consumed + blen > piece.text.size())
                    blen = 1;
                const std::size_t w = cp_width(piece.text.substr(consumed, blen));
                if (used + w > cols && !row.empty()) {
                    flush();
                    emitted_for_row = false; // new row: re-assert styling
                }
                if (!emitted_for_row) {
                    row += piece.sgr;
                    row_sgr += piece.sgr;
                    emitted_for_row = true;
                }
                row.append(piece.text, consumed, blen);
                used += w;
                consumed += blen;
            }
        }
        flush();
    }
    return rows;
}

std::string clip(std::string text, std::size_t cols) {
    if (display_width(text) <= cols)
        return text;
    std::string out;
    std::size_t used = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (static_cast<unsigned char>(text[i]) == 0x1B) {
            std::size_t j = i + 1;
            if (j < text.size() && text[j] == '[') {
                ++j;
                while (j < text.size()) {
                    const unsigned char d = static_cast<unsigned char>(text[j]);
                    ++j;
                    if (d >= 0x40 && d <= 0x7E)
                        break;
                }
            }
            out.append(text.substr(i, j - i));
            i = j;
            continue;
        }
        std::size_t len = 1;
        decode(text, i, &len);
        if (i + len > text.size())
            break;
        const std::size_t w = cp_width(text.substr(i, len));
        if (used + w + 3 /* room for ellipsis */ > cols)
            break;
        out.append(text.substr(i, len));
        used += w;
        i += len;
    }
    out += "...";
    return out;
}

// ---------------------------------------------------------------- renderer
//
// md4c (vendored MIT, third_party/md4c v0.6.0, ADR 004) drives a callback
// writer emitting ANSI SGR. Geometry (display_width/wrap/clip) stays ours.

} // namespace mini_agent::md

#include "md4c.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>

namespace mini_agent::md {
namespace {

struct TableModel {
    std::vector<std::size_t> align;             // per column MD_ALIGN (from head row)
    std::vector<std::vector<std::string>> rows; // first row is the header
    std::vector<std::string> current_row;
    bool in_head = false;
};

struct SgrWriter {
    std::string out;
    std::size_t cols = 80;

    int quote_depth = 0;
    std::vector<std::string> list_stack; // "ul" | "ol" per open list
    std::vector<std::size_t> ol_counters;
    bool at_line_start = true;
    bool block_open = false;
    bool in_code = false;
    // v5 (design 011 5.6): active fence language + cross-line highlight
    // carry; reset when a code block opens/closes.
    std::string code_lang;
    hl::State hl_state;
    unsigned heading_level = 0;
    std::string pending_marker; // emitted with the first text of an item
    bool heading_started = false;

    // table capture (cells stored PLAIN: grid math needs honest widths)
    bool table_mode = false;
    std::unique_ptr<TableModel> table;
    std::string cell_buf;
    std::size_t cell_align = 0;

    std::string indent() const {
        std::string s;
        for (int i = 0; i < quote_depth; ++i)
            s += "\xe2\x94\x82 "; // │ (dim applied by caller-less plain indent)
        s += pending_marker;
        return s;
    }

    void newline_if_open() {
        if (block_open) {
            out += "\x1b[0m\n";
            block_open = false;
            at_line_start = true;
        }
    }

    void text(std::string_view s) {
        if (s.empty())
            return;
        if (table_mode) {
            cell_buf.append(s);
            return;
        }
        if (at_line_start) {
            for (int i = 0; i < quote_depth; ++i)
                out += "\x1b[2m│\x1b[0m ";
            // outer list levels indent 2 columns each; the innermost level
            // is represented by pending_marker (bullet / number / checkbox)
            for (std::size_t level = 0; level + 1 < list_stack.size(); ++level) {
                out += "  ";
            }
            if (!pending_marker.empty()) {
                out += pending_marker;
                pending_marker.clear();
            }
            at_line_start = false;
        }
        out.append(s);
        block_open = true;
    }
};

std::string decode_entity(std::string_view e) {
    static const std::map<std::string, std::string> named = {
        {"&amp;", "&"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&nbsp;", " "},
        {"&mdash;", "\xe2\x80\x94"},
        {"&ndash;", "\xe2\x80\x93"},
        {"&hellip;", "\xe2\x80\xa6"},
        {"&copy;", "(c)"},
        {"&reg;", "(r)"},
        {"&trade;", "(tm)"},
    };
    if (auto it = named.find(std::string(e)); it != named.end())
        return it->second;
    std::uint32_t cp = 0;
    const bool hex = e.size() > 3 && (e[2] == 'x' || e[2] == 'X');
    for (std::size_t k = hex ? 3 : 2; k + 1 < e.size(); ++k) {
        const char c = static_cast<char>(e[k]);
        if (hex) {
            if (c >= '0' && c <= '9')
                cp = cp * 16 + static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                cp = cp * 16 + static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                cp = cp * 16 + static_cast<std::uint32_t>(c - 'A' + 10);
        } else if (c >= '0' && c <= '9') {
            cp = cp * 10 + static_cast<std::uint32_t>(c - '0');
        }
    }
    std::string utf;
    if (cp < 0x80)
        utf.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        utf.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        utf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        utf.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        utf.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        utf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        utf.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        utf.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        utf.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        utf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return utf;
}

void render_table(SgrWriter& w, const TableModel& model) {
    if (model.rows.empty())
        return;
    std::vector<std::size_t> widths(model.align.size(), 0);
    for (const auto& row : model.rows) {
        for (std::size_t c = 0; c < row.size() && c < widths.size(); ++c) {
            widths[c] = std::max(widths[c], display_width(row[c]));
        }
    }
    const std::string sep = " \x1b[2m|\x1b[0m ";
    bool header_done = false;
    for (const auto& row : model.rows) {
        std::string line;
        for (std::size_t c = 0; c < widths.size(); ++c) {
            const std::string cell = c < row.size() ? row[c] : std::string();
            const std::size_t pad =
                widths[c] >= display_width(cell) ? widths[c] - display_width(cell) : 0;
            std::size_t left = 0;
            std::size_t right = pad;
            if (model.align[c] == MD_ALIGN_CENTER) {
                left = pad / 2;
                right = pad - left;
            } else if (model.align[c] == MD_ALIGN_RIGHT) {
                left = pad;
                right = 0;
            }
            line.append(left, ' ');
            line += cell;
            line.append(right, ' ');
            if (c + 1 < widths.size())
                line += sep;
        }
        w.out += line + "\n";
        if (!header_done) {
            std::string rule;
            for (std::size_t c = 0; c < widths.size(); ++c) {
                for (std::size_t k = 0; k < widths[c]; ++k)
                    rule += "\xe2\x94\x80"; // ─
                if (c + 1 < widths.size())
                    rule += sep;
            }
            w.out += "\x1b[2m" + rule + "\x1b[0m\n";
            header_done = true;
        }
    }
    w.at_line_start = true;
    w.block_open = false;
}

// Signatures fixed by md4c's MD_PARSER callbacks.
int enter_block(MD_BLOCKTYPE type, void* detail, // NOLINT(bugprone-easily-swappable-parameters)
                void* userdata) {
    auto& w = *static_cast<SgrWriter*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_QUOTE:
        w.newline_if_open();
        ++w.quote_depth;
        break;
    case MD_BLOCK_UL: {
        w.newline_if_open();
        w.list_stack.push_back("ul");
        w.at_line_start = true;
        break;
    }
    case MD_BLOCK_OL: {
        const auto* d = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
        w.newline_if_open();
        w.list_stack.push_back("ol");
        w.ol_counters.push_back(d->start > 0 ? static_cast<std::size_t>(d->start) : 1u);
        w.at_line_start = true;
        break;
    }
    case MD_BLOCK_LI: {
        const auto* d = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
        w.newline_if_open();
        w.at_line_start = true;
        if (d->is_task) {
            const bool done = d->task_mark == 'x' || d->task_mark == 'X';
            w.pending_marker = done ? "\x1b[32m☑ \x1b[0m" : "\x1b[2m☐ \x1b[0m";
        } else if (!w.list_stack.empty() && w.list_stack.back() == "ol" && !w.ol_counters.empty()) {
            w.pending_marker = "\x1b[33m" + std::to_string(w.ol_counters.back()) + ".\x1b[0m ";
            ++w.ol_counters.back();
        } else {
            w.pending_marker = "\x1b[33m•\x1b[0m ";
        }
        break;
    }
    case MD_BLOCK_HR: {
        w.newline_if_open();
        w.at_line_start = true;
        w.text(""); // no-op keeps state hooks simple
        std::size_t n = std::min<std::size_t>(w.cols, 72);
        std::string dash;
        for (std::size_t k = 0; k < n; ++k)
            dash += "\xe2\x94\x80";
        w.out += "\x1b[2m" + dash + "\x1b[0m\n";
        w.block_open = false;
        break;
    }
    case MD_BLOCK_H: {
        const auto* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
        w.newline_if_open();
        w.at_line_start = true;
        w.heading_level = d->level;
        w.heading_started = false;
        break;
    }
    case MD_BLOCK_CODE: {
        w.newline_if_open();
        w.in_code = true;
        w.at_line_start = true;
        // v5 (design 011 5.6): fence info string selects the highlighter;
        // unknown/plain fences stay dim-indented as before.
        w.hl_state.reset();
        w.code_lang.clear();
        const auto* d = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
        if (d && d->lang.text != nullptr && d->lang.size > 0)
            w.code_lang = hl::language_for_fence(std::string_view(d->lang.text, d->lang.size));
        break;
    }
    case MD_BLOCK_P:
        w.newline_if_open();
        w.at_line_start = true;
        break;
    case MD_BLOCK_HTML:
        w.newline_if_open(); // raw HTML: skip content, keep layout stable
        break;
    case MD_BLOCK_TABLE:
        w.newline_if_open();
        w.table = std::make_unique<TableModel>();
        w.table_mode = true;
        break;
    case MD_BLOCK_THEAD:
        if (w.table)
            w.table->in_head = true;
        break;
    case MD_BLOCK_TBODY:
        if (w.table)
            w.table->in_head = false;
        break;
    case MD_BLOCK_TR:
        if (w.table)
            w.table->current_row.clear();
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        const auto* d = static_cast<const MD_BLOCK_TD_DETAIL*>(detail);
        w.cell_buf.clear();
        w.cell_align = d ? static_cast<std::size_t>(d->align) : 0;
        break;
    }
    case MD_BLOCK_BLANK:
        w.newline_if_open();
        break;
    default:
        break;
    }
    return 0;
}

int leave_block(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto& w = *static_cast<SgrWriter*>(userdata);
    switch (type) {
    case MD_BLOCK_QUOTE:
        w.newline_if_open();
        if (w.quote_depth > 0)
            --w.quote_depth;
        break;
    case MD_BLOCK_UL:
        w.newline_if_open();
        if (!w.list_stack.empty())
            w.list_stack.pop_back();
        break;
    case MD_BLOCK_OL:
        w.newline_if_open();
        if (!w.list_stack.empty())
            w.list_stack.pop_back();
        if (!w.ol_counters.empty())
            w.ol_counters.pop_back();
        break;
    case MD_BLOCK_LI:
        w.newline_if_open();
        break;
    case MD_BLOCK_H:
        w.out += "\x1b[0m\n";
        w.block_open = false;
        w.at_line_start = true;
        w.heading_level = 0;
        break;
    case MD_BLOCK_CODE:
        if (!w.out.empty() && w.out.back() != '\n')
            w.out += "\x1b[0m";
        if (!w.out.empty() && w.out.back() != '\n')
            w.out += "\n";
        w.in_code = false;
        w.block_open = false;
        w.at_line_start = true;
        break;
    case MD_BLOCK_P:
        w.newline_if_open();
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (w.table) {
            const std::size_t col = w.table->current_row.size();
            if (type == MD_BLOCK_TH && w.table->in_head) {
                if (col >= w.table->align.size()) {
                    w.table->align.resize(col + 1, MD_ALIGN_DEFAULT);
                }
                w.table->align[col] = w.cell_align;
            }
            w.table->current_row.push_back(w.cell_buf);
        }
        break;
    case MD_BLOCK_TR:
        if (w.table && !w.table->current_row.empty()) {
            w.table->rows.push_back(w.table->current_row);
        }
        break;
    case MD_BLOCK_TABLE:
        w.table_mode = false;
        if (w.table) {
            render_table(w, *w.table);
            w.table.reset();
        }
        break;
    default:
        break;
    }
    return 0;
}

int enter_span(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto& w = *static_cast<SgrWriter*>(userdata);
    if (w.table_mode)
        return 0; // cells stay plain for width math
    switch (type) {
    case MD_SPAN_EM:
        w.out += "\x1b[3m";
        break;
    case MD_SPAN_STRONG:
        w.out += "\x1b[1m";
        break;
    case MD_SPAN_DEL:
        w.out += "\x1b[9m";
        break;
    case MD_SPAN_CODE:
        w.out += "\x1b[7m";
        break;
    case MD_SPAN_A:
        w.out += "\x1b[1;34m";
        break; // bold blue, URL dropped
    case MD_SPAN_IMG:
        w.out += "\x1b[35m";
        break;
    default:
        break;
    }
    return 0;
}

int leave_span(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto& w = *static_cast<SgrWriter*>(userdata);
    if (w.table_mode)
        return 0;
    switch (type) {
    case MD_SPAN_EM:
        w.out += "\x1b[23m";
        break;
    case MD_SPAN_STRONG:
        w.out += "\x1b[22m";
        break;
    case MD_SPAN_DEL:
        w.out += "\x1b[29m";
        break;
    case MD_SPAN_CODE:
        w.out += "\x1b[27m";
        break;
    case MD_SPAN_A: // both clear their own open SGR
    case MD_SPAN_IMG:
        w.out += "\x1b[0m";
        break;
    default:
        break;
    }
    return 0;
}

// With MD_FLAG_NOHTML, md4c reports raw HTML (block or inline) as ordinary
// text. Strip <...> tag spans so markup never reaches the terminal.
std::string strip_html_tags(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            const auto close = s.find('>', i + 1);
            if (close != std::string_view::npos) {
                i = close + 1; // drop the tag
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

int process_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& w = *static_cast<SgrWriter*>(userdata);
    const std::string_view s(text, static_cast<std::size_t>(size));
    switch (type) {
    case MD_TEXT_NULLCHAR:
        return 0;
    case MD_TEXT_BR:
        if (w.in_code) {
            w.out += "\n";
            w.at_line_start = true;
        } else if (!w.table_mode) {
            w.out += "\n";
            w.block_open = false;
            w.at_line_start = true;
        }
        return 0;
    case MD_TEXT_SOFTBR:
        w.text(" ");
        return 0;
    case MD_TEXT_ENTITY: {
        const auto decoded = decode_entity(s);
        w.text(decoded);
        return 0;
    }
    case MD_TEXT_CODE:
        if (w.in_code) {
            // code block body: dim + two-space indent per physical line.
            // v5 (design 011 5.6): recognized fences highlight instead of
            // staying dim -- strings/comments/keywords carry their own SGR
            // (Run emits balanced resets, so wrap() accounting holds).
            const bool lit = !w.code_lang.empty();
            std::size_t pos = 0;
            while (pos < s.size()) {
                const auto nl = s.find('\n', pos);
                const auto end = nl == std::string_view::npos ? s.size() : nl;
                if (end > pos) {
                    if (w.at_line_start) {
                        for (int i = 0; i < w.quote_depth; ++i)
                            w.out += "\x1b[2m│\x1b[0m ";
                        if (!lit)
                            w.out += "\x1b[2m  ";
                        else
                            w.out += "  ";
                        w.at_line_start = false;
                    }
                    if (lit)
                        w.out +=
                            hl::highlight_line(s.substr(pos, end - pos), w.code_lang, w.hl_state);
                    else
                        w.out.append(s.substr(pos, end - pos));
                    if (!lit)
                        w.out += "\x1b[0m";
                    w.block_open = true;
                }
                if (nl != std::string_view::npos) {
                    w.out += "\n";
                    w.block_open = false;
                    w.at_line_start = true;
                }
                pos = end + (nl == std::string_view::npos ? 0 : 1);
            }
        } else {
            w.text(s); // inline code: SPAN_CODE SGR already opened
        }
        return 0;
    case MD_TEXT_HTML:
        return 0; // raw HTML dropped (terminal display)
    case MD_TEXT_NORMAL: {
        if (w.heading_level > 0 && !w.table_mode && !w.heading_started) {
            w.out += w.heading_level <= 2 ? "\x1b[1;36m▍ " : "\x1b[1;36m";
            w.heading_started = true;
        }
        const auto cleaned = strip_html_tags(s);
        w.text(cleaned);
        return 0;
    }
    default:
        if (w.heading_level > 0 && !w.table_mode && !w.heading_started) {
            w.out += w.heading_level <= 2 ? "\x1b[1;36m▍ " : "\x1b[1;36m";
            w.heading_started = true;
        }
        w.text(s);
        return 0;
    }
}

} // namespace

std::string render(std::string_view markdown, std::size_t cols) {
    SgrWriter w;
    w.cols = cols > 0 ? cols : 80;

    MD_PARSER parser = {};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML;
    parser.enter_block = enter_block;
    parser.leave_block = leave_block;
    parser.enter_span = enter_span;
    parser.leave_span = leave_span;
    parser.text = process_text;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    std::string copy(markdown);
    md_parse(copy.data(), static_cast<MD_SIZE>(copy.size()), &parser, &w);
    w.newline_if_open();
    return w.out;
}

} // namespace mini_agent::md
