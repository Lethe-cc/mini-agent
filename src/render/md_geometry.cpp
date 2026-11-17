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
