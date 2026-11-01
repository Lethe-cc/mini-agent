#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/config.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"

namespace mini_agent::tui {

// Pure, testable helpers for the self-drawn TUI (design 007 section 5.6).

// Slash-command registry (v1 minimal set, contract 5.6).
std::vector<std::string_view> slash_commands() noexcept;
// Case-sensitive prefix matches among the registry, registry order kept.
std::vector<std::string_view> slash_matches(std::string_view prefix);
// Next cyclic Tab completion for an input starting with `/`; returns the
// replacement line text, or nullopt when nothing matches / input is not a
// slash line. Cycles per call so repeated Tab walks the list.
std::optional<std::string> slash_tab_complete(std::string_view line, int& cursor_state);
// Hint rows shown under the composer while it is a `/`-line, as a vertical
// list: header row then one row per prefix match Tab cycles through; the
// no-match notice instead when nothing matches. Empty for non-slash input
// (2026-11-01 maintainer requests; vertical form per follow-up).
std::vector<std::string> slash_completion_hint(std::string_view composer);

// UTF-8 aware composer editing: append validates/rejects lone surrogates
// only trivially (we accept opaque bytes); returns bytes appended.
std::size_t utf8_append(std::string& line, std::string_view bytes);
// Delete the last complete UTF-8 codepoint (continuation bytes 0x80-0xBF).
bool utf8_backspace(std::string& line) noexcept;
// Display width in codepoints (not columns; CJK width mapping is v2).
std::size_t utf8_codepoints(std::string_view text) noexcept;
// Cursor-aware editing (2026-11-01 arrow-key request): pos is a byte
// offset always kept at a codepoint boundary; next/prev snap past
// continuation bytes. insert writes bytes at pos and advances pos.
std::size_t utf8_next(std::string_view text, std::size_t pos) noexcept;
std::size_t utf8_prev(std::string_view text, std::size_t pos) noexcept;
void utf8_insert(std::string& line, std::size_t& pos, std::string_view bytes);
// erase_before deletes the codepoint left of pos (backspace at cursor);
// erase_at deletes the codepoint under pos (forward delete). Both return
// false when nothing was removed.
bool utf8_erase_before(std::string& line, std::size_t& pos) noexcept;
bool utf8_erase_at(std::string& line, std::size_t& pos) noexcept;

struct TuiOptions {
    std::string model; // display only; live value comes from config
    std::string session_id;
    std::size_t frame_ms = 50; // poll tick / max redraw cadence (5.6 rule 1)
};

// Interactive TUI over one session. Owns the raw-mode/termios lifecycle and
// the request worker; exits with an exit code. Persists per-turn saves via
// store (atomic + lock already provided). Returns process exit code.
// `config` is mutated by /login /model /provider inside the loop; a fresh
// provider is built from it at every turn start (design 007: no global
// singletons; multi-agent extension point 5.5).
int run_loop(AppConfig& config, Session& session, SessionStore& store, const TuiOptions& options);

} // namespace mini_agent::tui
