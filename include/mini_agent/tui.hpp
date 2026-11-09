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
std::optional<std::string> slash_tab_complete(std::string_view line, int& cursor_state,
                                              const std::vector<std::string>& session_ids = {});
// Hint rows shown under the composer while it is a `/`-line, as a vertical
// list: header row then one row per prefix match Tab cycles through; the
// no-match notice instead when nothing matches. Empty for non-slash input
// (2026-11-01 maintainer requests; vertical form per follow-up).
std::vector<std::string> slash_completion_hint(std::string_view composer,
                                               const std::vector<std::string>& session_ids = {});

// UTF-8 aware composer editing: append validates/rejects lone surrogates
// only trivially (we accept opaque bytes); returns bytes appended.
std::size_t utf8_append(std::string& line, std::string_view bytes);
// Delete the last complete UTF-8 codepoint (continuation bytes 0x80-0xBF).
bool utf8_backspace(std::string& line) noexcept;
// Display width in codepoints (not columns; CJK width mapping is v2).
std::size_t utf8_codepoints(std::string_view text) noexcept;
// Resolve a numbered selection against a fetched model list (design 007
// model-picker). `input` is the composer text at Enter while the list is
// shown: a 1-based index returns that model's id; an exact id match is
// accepted too; anything else (out of range, non-numeric, unknown id) is
// nullopt so the caller can keep picking or fall through to /model <name>.
std::optional<std::string> model_choice(const std::vector<std::string>& models,
                                        std::string_view input);
// Numbered menu rows for a fetched model list: a header, one `N. id` row per
// model (the `current` id tagged `<- current`), and a footer explaining the
// keys. Empty when the list is empty. Committed to the history area once the
// fetch completes; the user types a number (or /model <name>) to choose.
std::vector<std::string> model_menu_rows(const std::vector<std::string>& models,
                                         std::string_view current);
// Incremental configuration guidance for batched entry (2026-11-02 request):
// reports how many of base_url/api_key/model are set and names only the NEXT
// missing step in provider -> key -> model order, never the full command list
// (listing all three reads as "enter everything at once"). Returns
// `config 3/3 · ready` when complete. Values are never echoed -- only which
// fields are present -- so the api_key cannot leak through a hint line.
std::string config_next_hint(const AppConfig& config);

// Login/auth status. The model-list request fired right after /provider and
// /login doubles as an auth probe (no extra request): a provider that rejects
// the key answers 401/403, and any 2xx means the credentials were accepted
// even when the body shape then failed to parse. Deliberately NOT a second
// network call -- probing at every startup would burn the cold-start budget.
struct AuthProbe {
    bool attempted = false;     // a model-list or chat attempt has completed
    bool fetch_ok = false;      // it produced a usable response
    int http_status = 0;        // status code if one was received, else 0
    bool network_error = false; // failed before any response arrived
    std::size_t model_count = 0;
};

// Line-level history colouring (2026-11-04 readability request). Tag-
// prefixed system rows pick a colour from their leading marker and outcome:
// `[ok]` green, `[error]`/denied tools red, `[warn]`/`[tool]` errors yellow,
// `[info]`/`[input]` cyan, `[auth]` per verdict, banners bold, key-hint and
// config-echo rows dim. The SGR wraps the WHOLE line, so every printable
// substring stays contiguous for copy/paste and test assertions. Lines that
// already carry ESC bytes (markdown-rendered replies, producer-styled rows)
// are returned untouched -- styling there belongs to the producer. When
// `colors` is false (NO_COLOR, non-TTY stdout) the line is unchanged.
std::string style_history_line(std::string_view line, bool colors);
// Whether history/live styling may emit SGR: a TTY on stdout and NO_COLOR
// unset (https://no-color.org). Process-lifetime constant, cached.
bool colors_enabled();

// One-line, user-facing (English per design 007 section 5.6) auth status.
// Never echoes any part of the key.
std::string auth_status_line(const AuthProbe& probe);
// Single log token: untried | verified | rejected | unreachable | refused.
std::string_view auth_log_token(const AuthProbe& probe);

// Provider connection state, derived (never probed) from the config plus the
// last model-list probe and whether a probe is in flight (2026-11-02 maintainer
// request: the `>` prompt itself should carry the connectivity indicator).
enum class ConnState {
    Unconfigured, // base_url or api_key missing
    Checking,     // a probe request is in flight right now
    Verified,     // last probe accepted the credentials
    Rejected,     // last probe answered 401/403
    Unreachable,  // probed, but no usable response arrived at all
    Unverified,   // configured but not probed yet (or a non-auth status code)
};
ConnState connection_state(const AppConfig& config, const AuthProbe& probe, bool listing) noexcept;
// The styled composer prompt marker shown in place of the plain `>` (one
// display column, so the hard-wrap row accounting of 5.6 rule 2 is unaffected:
// md::wrap treats SGR as zero width). Glyph AND colour encode the state, so
// monochrome terminals and colour-blind users still get the signal:
//   `?` dim unconfigured · `?` yellow unverified · `~` cyan checking ·
//   `>` green verified · `!` red rejected · `x` magenta unreachable.
// Carries no text at all, so no endpoint, key, or status code can leak here.
std::string prompt_marker(ConnState state) noexcept;
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
    // v4.15: how the session was chosen, shown as a tag on the banner/
    // status session id: "temp" (this options flag) > "resumed" (loaded
    // from the store) > "new" (freshly created by this launch).
    bool resumed = false;
    // v4.15 (--temp, maintainer request): the session is ephemeral. Nothing
    // is ever written to the store (no per-turn save, no save before a
    // `/session` switch) and the exit line says the chat was discarded
    // instead of offering a resume hint. Inside a temp session `/session
    // new` keeps being temp; `/session <id>` resumes a saved session and
    // leaves temp mode.
    bool temp = false;
};

// ---------------------------------------------------------------- v2 tools

// One history-area line for a tool event: `[tool] name · summary` plus
// outcome `[ok 1.2s · 340 B]` / `[error]` (design 008 section 5.4). Text
// is sanitized and collapsed to one row so the frame model stays intact.
std::string tool_activity_line(const std::string& tool_name, const std::string& arguments_json,
                               bool executed, bool is_error, long long elapsed_ms,
                               std::size_t result_bytes);

// v4.11 (design 010 §21): rows for the in-turn activity panel -- the
// process view between "question sent" and "answer complete" (route A of
// the right-column discussion: the inline frame cannot split columns, so
// the panel lives in the live region's extra-rows slot while streaming).
// Pure: the caller snapshots the turn state; rows are pre-tinted or plain
// depending on `colors`. Layout: header ("this turn Ns"), a thinking
// marker when the gateway has streamed reasoning deltas this turn, then
// the newest tool rows (name · summary [state]) capped at max_rows with
// the OLDEST dropped (the running call is the interesting one).
std::vector<std::string> turn_panel_rows(bool colors, bool thinking, long long elapsed_secs,
                                         const std::vector<std::string>& tool_rows,
                                         std::size_t max_rows);

// v4.13 (design 007 §v4.13): right-hand sidebar inside the inline frame.
// compose_columns merges pre-built content rows (width <= cols-side_w-3)
// with sidebar rows (width <= side_w) into full-width rows joined by a
// " | " separator; content rows are padded to the split width with
// spaces (display-width aware). Rows beyond the sidebar's length render
// blank on the right; shorter content is what the caller guarantees
// (md::clip). Pure + unit-testable; the caller owns data gathering.
std::vector<std::string> compose_columns(const std::vector<std::string>& content,
                                         const std::vector<std::string>& sidebar, std::size_t cols,
                                         std::size_t side_w);

// One-line git porcelain status glyph for the sidebar ("?? " -> "??",
// " M " -> "M", "A " -> "A", "R.." -> "R"); empty on unparseable input.
std::string sidebar_status_glyph(std::string_view xy);

// v4.16: merge a left-aligned status line and a right-aligned info line
// into ONE bottom row: `left` is clipped (with an ellipsis budget) so
// `right` always lands flush against the right edge; a single space is
// the minimum gutter. Returns the composed row (display-width exact).
std::string compose_footer(std::string_view left, std::string_view right, std::size_t cols);

// v4.12: one bottom system-info row: "user@host · cwd · clock" (dim when
// colors on). Pure: the caller gathers the values (USER/getpwuid,
// gethostname, workspace-or-cwd with $HOME abbreviated to ~, local
// HH:MM:SS). Inputs are sanitized here so no escape bytes reach the
// frame (5.6 rule 2).
std::string sysinfo_line(bool colors, const std::string& user, const std::string& host,
                         const std::string& cwd, const std::string& clock);

// Compact one-row summary of a tool call's primary argument (command/
// path/text/url/query), sanitized + clipped. Shared by the history line
// and the panel.
std::string tool_arg_summary(const std::string& arguments_json);

// Approval key state machine (pure; main thread drives it). `expanded`
// toggles the full-arguments view with `?`. Returns the action for the
// key or nullopt when the key belongs to the normal composer.
enum class ApprovalKey { Allow, Deny, AllowSession };
std::optional<ApprovalKey> approval_key(int ch, bool& expanded) noexcept;

// Rows shown in the live region while an approval is pending: the prompt
// row (command/argument summary) plus the choices legend; when expanded,
// pretty-printed argument JSON lines follow.
std::vector<std::string> approval_rows(const std::string& tool_name,
                                       const std::string& arguments_json, bool expanded,
                                       const std::string& preview = {});

// Registry dump rows for /tools: "name · source · description-first-line"
// one row per tool (sanitized), header + count footer.
std::vector<std::string> tool_registry_rows(const std::vector<std::string>& rendered_tools);

// ---------------------------------------------------------------- v4 status

// Context-window / reasoning-effort configuration values (design 010).
struct StatusInputs {
    long long context_tokens = 0; // last prompt size (usage or estimate)
    bool context_estimated = false;
    long long context_window = 0;    // config; 0 = unset
    long long completion_tokens = 0; // session sum (0 = nothing reported yet)
    std::string reasoning_effort;    // config; empty = unset
    // v4.2 (design 010 §11): reasoning ACTUALLY observed in responses.
    // v4.2.1 refresh semantics: reflects the LAST successful turn only.
    // Display preference: echoed level > configured level confirmed by
    // evidence > "on" (evidence, no level known) > config echo (intent,
    // unconfirmed).
    bool reasoning_seen = false;
    std::string reasoning_actual;
};
// Tinted trailing segments for the bottom status row: `ctx 45.2k/128.0k`
// (used/max, coloured by the 70%/90% thresholds; `~` prefix when
// estimated; single value when no window is configured), `out 1.2k`, and
// `reasoning=<level>`. Empty segments are omitted entirely; returns ""
// when nothing to show so the pre-v4 status row stays byte-identical.
// Colour codes follow the existing `tint` scheme (2 dim default, 33
// yellow, 31 red).
std::string status_usage_segments(bool colors, const StatusInputs& inputs);

// v4.2: numbered menu rows for a model's supported reasoning efforts
// (design 010 §11). Empty when the list is empty. Marks the gateway's
// default effort `<- default` and the current value `<- current`; the
// footer notes mandatory-reasoning models. Selection reuses model_choice()
// against the efforts vector.
std::vector<std::string> reasoning_menu_rows(const std::vector<std::string>& efforts,
                                             std::string_view current,
                                             std::string_view default_effort, bool mandatory);

// Compact, human-readable token counts for every user-facing display
// (status bar, /context readouts, probe results): <1k raw, <1M one-
// decimal k, <1G one-decimal M, else one-decimal G. Pi-style ladder.
std::string format_token_count(long long n);

// Interactive TUI over one session. Owns the raw-mode/termios lifecycle and
// the request worker; exits with an exit code. Persists per-turn saves via
// store (atomic + lock already provided). Returns process exit code.
// `config` is mutated by /login /model /provider inside the loop; a fresh
// provider is built from it at every turn start (design 007: no global
// singletons; multi-agent extension point 5.5).
int run_loop(AppConfig& config, Session& session, SessionStore& store, const TuiOptions& options);

} // namespace mini_agent::tui
