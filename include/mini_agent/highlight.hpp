#pragma once

#include <string>
#include <string_view>

namespace mini_agent::hl {

// v5 (design 011): lightweight syntax highlighting for the human preview
// paths (Markdown code fences, /preview, sidebar p key). Vim-style line
// lexical model: no parser library (ADR 003 zero-dependency posture).
// Per line we classify comments, strings, numbers, keywords,
// preprocessor lines and function calls into a handful of SGR classes.
// Cross-line constructs (C block comments, Python triple-quoted strings)
// carry through an explicit State so line-at-a-time callers stay correct.
// Pure functions, unit-testable; output carries SGR unconditionally --
// callers gate on terminal capability (same contract as md::render).

// Opaque cross-line carry (block comment / open triple quote). Copyable.
struct State {
    // 0 = normal; 1 = inside a block comment (cpp family); 2 = inside a
    // triple-quoted string (python family); quote records the opening
    // character.
    int open = 0;
    char quote = 0;

    void reset() noexcept {
        open = 0;
        quote = 0;
    }
};

// Language id ("cpp" | "py" | "sh" | "json" | "js") from a file path or
// name by extension; empty string when unknown/unsupported.
std::string language_for_path(std::string_view path);

// Language id from a Markdown fence info string (cpp, python, ...);
// aliases normalized; empty when unknown/unsupported or a plain fence.
std::string language_for_fence(std::string_view info);

// True when highlight_line does more than pass text through for lang.
bool supports(std::string_view lang);

// One line -> terminal line. line must already be sanitized (no ESC
// bytes): the function adds its own SGR only. UTF-8 passes through
// byte-wise (tokens are ASCII; multi-byte content lands in plain runs).
// On empty/unknown lang the input is returned unchanged. Advances state
// across calls for one file: feed whole lines in order.
std::string highlight_line(std::string_view line, std::string_view lang, State& state);

} // namespace mini_agent::hl
