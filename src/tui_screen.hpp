#pragma once
// Internal TUI screen/terminal layer (design 019 P3): RawMode, term size,
// SGR tint, LiveScreen frame renderer, ANSI paragraph split, steady_now.
// Moved verbatim from tui.cpp; included only by tui.cpp (single TU).
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "mini_agent/md.hpp"

namespace mini_agent::tui {
class RawMode {
  public:
    explicit RawMode(int fd) {
        if (::tcgetattr(fd, &saved_) == 0) {
            struct termios raw = saved_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
            // ISIG off: ^C/^D arrive as 0x03/0x04 bytes for our keymap
            // (design 007 section 5.6); SIGINT is ignored at entry anyway.
            raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL); // we see \r ourselves
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            active_ = ::tcsetattr(fd, TCSANOW, &raw) == 0;
        }
    }
    ~RawMode() {
        if (active_)
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

  private:
    struct termios saved_{};
    bool active_ = false;
};

inline int term_columns() {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

inline int term_rows() {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return ws.ws_row;
    }
    return 24;
}

inline bool starts_with(std::string_view line, std::string_view prefix) noexcept {
    return line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0;
}

inline std::string tint(bool colors, std::string_view sgr, std::string_view text) {
    if (!colors)
        return std::string(text);
    std::string out;
    out.reserve(text.size() + sgr.size() + 6);
    out += "\x1b[";
    out += sgr;
    out += 'm';
    out += text;
    out += "\x1b[0m";
    return out;
}

class LiveScreen {
  public:
    // One history row: hard-wrapped by us at `cols` display columns, so
    // cursor accounting never depends on the terminal's own auto-wrap
    // (fixes 2026-11-01 duplicate-row reports: CJK double-width and
    // multi-line replies both desynced the old relative-rows model).
    void commit(std::string_view line) {
        clear_live(); // History colouring (2026-11-04 readability request): classify the
        // tag/role of plain lines centrally, so every caller gets consistent
        // hues without threading a flag through each commit site. Rows that
        // already carry SGR (markdown replies, producer-styled registry/
        // approval/tool rows) pass through untouched -- no double styling.
        // md::wrap treats the spans as zero-width (5.6 rule 2 accounting is
        // unchanged), and commit lines are hard-wrapped, not clipped, so the
        // line may exceed the clipped width of the live status row here.
        const std::string styled = style_history_line(line, colors_enabled());
        // Wrap budget cols-1: a row reaching the last column triggers the
        // terminal's deferred autowrap on the trailing \n (two rows advanced
        // for one printed row), breaking the live-block accounting.
        const std::size_t cols = current_cols() > 1 ? current_cols() - 1 : 1;
        std::size_t printed = 0;
        std::string out;
        for (auto& row : md::wrap(styled, cols)) {
            out += "\x1b[2K" + row + "\n";
            ++printed;
        }
        ::fwrite(out.data(), 1, out.size(), stdout);
        ::fflush(stdout);
        last_frame_.clear();
        prev_rows_.clear();
        inputs_cached_ = false;
        // Bottom-anchor tracking (2026-11-05): commit prints at the block
        // top (clear_live left the cursor there), so the next block starts
        // `printed` rows lower; past the bottom the terminal scrolls and
        // the cursor rests on the last row.
        const std::size_t rows = current_rows();
        block_top_ = std::min(block_top_ + printed, rows > 0 ? rows - 1 : 0);
    }

    // Repaint the live block (pinned question + streaming tail window +
    // slash hint + composer + status).
    // 2026-11-01 "full-terminal refresh": the tail window grows with the terminal
    // height instead of a fixed 6 rows, so streaming output refreshes
    // across (almost) the whole screen and slides like a scroll. Row
    // accounting invariant (5.6 rule 2) holds because the block is
    // capped at rows-1 lines and every frame ends with the cursor one
    // row below the block: the block top can never scroll above screen
    // row 1, so the `\x1b[nA` back-up stays exact and no stale rows
    // survive (shrink below the block is explicitly cleared).
    // 2026-11-01 "pin the question on top while streaming": while streaming, the submitted
    // question is re-shown as the block's first rows (`pinned_question`),
    // because it only enters history at turn commit and would otherwise
    // vanish from the refreshed region.
    void render(const std::string& response_tail, const std::string& pinned_question,
                const std::string& composer, const std::string& composer_display,
                const std::string& prompt, const std::string& status, bool streaming,
                std::size_t cols, const std::vector<std::string>& extra_rows = {},
                const std::string& sysinfo = {}, const std::vector<std::string>& session_ids = {},
                const std::vector<std::string>& sidebar = {},
                std::size_t sidebar_sel = static_cast<std::size_t>(-1),
                const std::vector<std::string>& provider_names = {},
                const std::vector<std::string>& model_choices =
                    {}) { // NOLINT(bugprone-easily-swappable-parameters)
        const std::size_t rows = current_rows();
        // Slash hint (2026-11-01 maintainer request; vertical list per
        // follow-up): while the composer is a slash line, enumerate Tab
        // matches one row each. Derived from the RAW composer -- the
        // display string carries caret/mask decoration that must not
        // affect matching. Empty raw string (masked input) never hints.
        // Pi requestRender pattern (2026-11-01): identical render inputs
        // cannot produce a different frame, so an unchanged poll tick does
        // zero wrap/alloc/compare work. The cache is invalidated by
        // commit()/clear_live(), which move the live block behind our back,
        // and by terminal height changes (window budget follows rows).
        if (inputs_cached_ && rows == last_rows_ && cols == last_cols_ &&
            streaming == last_streaming_ && response_tail == last_tail_ &&
            pinned_question == last_pin_ && composer == last_composer_ &&
            composer_display == last_comp_disp_ && prompt == last_prompt_ &&
            status == last_status_ && extra_rows == last_extra_ && sysinfo == last_sysinfo_ &&
            session_ids == last_session_ids_ && sidebar == last_sidebar_ &&
            sidebar_sel == last_sidebar_sel_ && provider_names == last_provider_names_ &&
            model_choices == last_model_choices_) {
            return;
        }
        inputs_cached_ = true;
        last_rows_ = rows;
        last_cols_ = cols;
        last_streaming_ = streaming;
        last_tail_ = response_tail;
        last_pin_ = pinned_question;
        last_composer_ = composer;
        last_comp_disp_ = composer_display;
        last_prompt_ = prompt;
        last_status_ = status;
        last_extra_ = extra_rows;
        last_sysinfo_ = sysinfo;
        last_session_ids_ = session_ids;
        last_sidebar_ = sidebar;
        last_sidebar_sel_ = sidebar_sel;
        last_provider_names_ = provider_names;
        last_model_choices_ = model_choices;
        // v4.16: one merged footer row (status + right-aligned sysinfo)
        // on top of the cursor row.
        const std::size_t fixed_rows = 2;
        // v4.13: with the sidebar active the content column shrinks to
        // cols - side_w - 3 (" | " gutter); every content row wraps to
        // that width, then composes 1:1 with a sidebar row. Composed rows
        // are exactly one terminal row each (left padded by display
        // width, right clipped), preserving the hard-wrap invariant
        // (5.6 rule 2). Narrow terminals (< 60 cols) shrink the sidebar.
        const std::size_t side_w = sidebar.empty() ? 0 : (cols > 60 ? 30 : cols / 3);
        // Content wraps to cols-1 (autowrap guard, see compose_footer); with
        // the sidebar the content column shrinks by side_w + 3 (" | " gutter).
        const std::size_t content_cols =
            side_w > 0 && cols > side_w + 4 ? cols - side_w - 4 : (cols > 1 ? cols - 1 : 1);
        const std::size_t cap = rows > fixed_rows ? rows - fixed_rows : 1;
        auto pin_rows =
            streaming ? md::wrap(style_history_line("You: " + pinned_question, colors_enabled()),
                                 content_cols)
                      : std::vector<std::string>{};
        auto tail_rows =
            streaming ? md::wrap(response_tail, content_cols) : std::vector<std::string>{};
        // The prompt marker carries the provider connection state (2026-11-02
        // request): same row as the composer, one display column wide.
        auto comp_rows = md::wrap(prompt + " " + composer_display, content_cols);
        // Row budget, priority low->high trimmed: status (fixed, appended
        // below), then tail, then the hint list, then the pinned question,
        // keeping the composer whole (2026-11-01: the question stays
        // visible during output, so it outranks the sliding tail window).
        std::size_t budget = cap;
        trim_last(comp_rows, budget);
        budget -= std::min(budget, comp_rows.size());
        trim_last(pin_rows, budget);
        budget -= std::min(budget, pin_rows.size());
        // Vertical list keeps its header when short on rows: build from
        // the top and stop at the budget (opposite of trim_last).
        std::vector<std::string> hint_rows;
        std::vector<std::string> hint_source;
        if (!extra_rows.empty()) {
            hint_source = extra_rows; // approval block outranks slash hints
        } else {
            hint_source =
                slash_completion_hint(composer, session_ids, provider_names, model_choices);
        }
        for (const auto& line : hint_source) {
            if (hint_rows.size() >= budget)
                break;
            for (auto& wrapped : md::wrap(line, content_cols)) {
                if (hint_rows.size() >= budget)
                    break;
                hint_rows.push_back(std::move(wrapped));
            }
        }
        budget -= hint_rows.size();
        trim_last(tail_rows, budget);
        std::vector<std::string> content;
        content.insert(content.end(), pin_rows.begin(), pin_rows.end());
        content.insert(content.end(), tail_rows.begin(), tail_rows.end());
        content.insert(content.end(), comp_rows.begin(), comp_rows.end());
        content.insert(content.end(), hint_rows.begin(), hint_rows.end());
        if (side_w > 0) {
            std::vector<std::string> sb = sidebar;
            if (sidebar_sel < sb.size()) // reverse video survives clip (zero-width)
                sb[sidebar_sel] = "\x1b[7m" + sb[sidebar_sel] + "\x1b[27m";
            content = compose_columns(content, sb, cols, side_w);
        }
        // Status is always one row; the trailing reset matters now that the
        // row carries SGR segments: md::clip can cut mid-span, and this is
        // the block's last row, so an unclosed span would leak into the next
        // repaint's rows (5.6 rule 2: no style may survive a row boundary).
        // v4.16: status left + system info right-aligned on ONE row
        // (the two-row footer of v4.12 read as clutter; the info stays
        // fully visible, the status clips with the width budget).
        // compose_footer caps the composed width at cols-1 (autowrap guard).
        content.push_back(compose_footer(md::clip(status, cols), md::clip(sysinfo, cols), cols) +
                          "\x1b[0m");
        // Bottom-anchored live block (2026-11-05 maintainer request): pad
        // the TOP with blank rows so the status row rests one line above
        // the screen bottom and the cursor sits on the last line, whether
        // idle or streaming. Padding is dynamic -- only the space between
        // the tracked block top and the bottom is filled, so committed
        // history stays visible until the screen genuinely fills, then the
        // terminal scrolls and we track the shifted top (rule 2 invariant
        // holds: blank rows are plain "\x1b[2K" lines, no styling leaks;
        // the cursor always ends exactly one row below the block).
        if (rows > 1) {
            const std::size_t used = block_top_ + content.size();
            if (used < rows - 1) {
                content.insert(content.begin(), (rows - 1) - used, std::string());
            } else if (used > rows - 1) {
                // Emitting this block scrolled the screen by the overflow:
                // the top moved up accordingly (clamped at row 0).
                const std::size_t scrolled = used - (rows - 1);
                block_top_ = block_top_ > scrolled ? block_top_ - scrolled : 0;
            }
        }

        const std::string joined = join(content);
        if (joined == last_frame_ && content.size() == live_rows_) {
            return; // nothing changed: skip redundant repaint
        }
        last_frame_ = joined;

        // Row-level diff (Pi TuiMainScreen pattern, 2026-11-01): rows
        // identical to the previous frame at the same block index are left
        // untouched (each row is reset-terminated by md::wrap, so skipping a
        // row cannot leak styling). Only the changed prefix onward is
        // rewritten; the shrink cleanup below stays byte-identical to the
        // pre-diff tested path, so the hard-wrap row accounting of rule 2 is
        // not re-opened to the duplicate-row class of bugs.
        std::size_t first_changed = 0;
        while (first_changed < content.size() && first_changed < prev_rows_.size() &&
               content[first_changed] == prev_rows_[first_changed]) {
            ++first_changed;
        }

        std::string out = "\x1b[?2026h"; // synchronized output: atomic frame
        // CSI 0 A means one row up, not zero: a new block must start at
        // the current cursor or its first repaint erases committed history.
        if (live_rows_ > 0)
            out += "\x1b[" + std::to_string(live_rows_) + "A";
        if (first_changed > 0) {
            out += "\x1b[" + std::to_string(first_changed) + "B";
        }
        out += "\r";
        for (std::size_t i = first_changed; i < content.size(); ++i) {
            out += "\x1b[2K" + content[i] + "\n";
        }
        if (live_rows_ > content.size()) {
            // Block shrank (e.g. multi-line composer cleared on Enter):
            // erase the stale rows left below the new block, then step
            // back to the line under its end.
            const std::size_t extra = live_rows_ - content.size();
            for (std::size_t i = 0; i < extra; ++i)
                out += "\x1b[2K\n";
            out += "\x1b[" + std::to_string(extra) + "A";
        }
        out += "\x1b[?2026l";
        ::fwrite(out.data(), 1, out.size(), stdout);
        ::fflush(stdout);
        live_rows_ = content.size();
        prev_rows_ = std::move(content);
    }

    void clear_live() {
        inputs_cached_ = false;
        if (live_rows_ == 0) {
            prev_rows_.clear();
            last_frame_.clear();
            return;
        }
        std::string out = "\x1b[" + std::to_string(live_rows_) + "A";
        for (std::size_t i = 0; i < live_rows_; ++i)
            out += "\x1b[2K\n";
        out += "\x1b[" + std::to_string(live_rows_) + "A";
        ::fwrite(out.data(), 1, out.size(), stdout);
        ::fflush(stdout);
        live_rows_ = 0;
        last_frame_.clear();
        prev_rows_.clear();
    }

  private:
    static std::size_t current_cols() {
        const int c = term_columns();
        return c > 0 ? static_cast<std::size_t>(c) : 80;
    }

    static std::size_t current_rows() {
        const int r = term_rows();
        return r > 0 ? static_cast<std::size_t>(r) : 24;
    }

    // Keep the last `keep` rows of a wrapped block (window slide).
    static void trim_last(std::vector<std::string>& v, std::size_t keep) {
        if (v.size() > keep) {
            v.erase(v.begin(), v.end() - static_cast<std::ptrdiff_t>(keep));
        }
    }

    std::size_t live_rows_ = 0;
    // Screen row (0-based) where the live block starts. Tracked across
    // commits (each commit prints at the block top and advances it) and
    // scrolls (render() subtracts the overflow the emit caused), so
    // render() can pad the top and keep the status row bottom-anchored
    // (2026-11-05). Terminal resize is self-correcting: a smaller height
    // makes the next frame see overflow and clamp the top back down; a
    // taller one just pads more. After a resize the exact anchor may be
    // off until the next commit re-advances the top (terminal reflow
    // moves the real cursor in ways we cannot compute without a cursor
    // position report); the block stays fully visible and correct either
    // way -- padding only shifts where the block starts, never what it
    // erases (live_rows_ accounting is independent).
    std::size_t block_top_ = 0;
    std::string last_frame_;             // repaint suppression cache; reset on commit/clear
    std::vector<std::string> prev_rows_; // previous frame's rows for the diff

    // render() input cache (Pi requestRender pattern): skip all work when
    // the poll tick produced identical inputs. Invalidated on every
    // commit()/clear_live() (block moved) and on rows/cols/streaming change.
    bool inputs_cached_ = false;
    std::size_t last_rows_ = 0;
    std::size_t last_cols_ = 0;
    bool last_streaming_ = false;
    std::string last_tail_;
    std::string last_pin_;
    std::string last_composer_;
    std::string last_comp_disp_;
    std::string last_prompt_;
    std::string last_status_;
    std::vector<std::string> last_extra_;
    std::string last_sysinfo_;
    std::vector<std::string> last_session_ids_;
    std::vector<std::string> last_sidebar_;
    std::size_t last_sidebar_sel_ = static_cast<std::size_t>(-1);
    std::vector<std::string> last_provider_names_;
    std::vector<std::string> last_model_choices_;

    static std::string join(const std::vector<std::string>& rows) {
        std::string out;
        for (const auto& r : rows) {
            out += r;
            out += '\x01';
        }
        return out;
    }
};

// LiveScreen is declared inside an anonymous namespace above; the free
// helpers need to be visible before it, so keep this region closed here.

// Split rendered markdown (may itself contain ANSI) into one commit per
// logical line, prefixing only the first line of the assistant block.
inline std::vector<std::string> split_ansi_paragraphs(const std::string& rendered) {
    std::vector<std::string> lines;
    std::istringstream stream(rendered);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        // "AI: " prefix (4 display cols); continuation rows indent to align.
        lines.push_back(first ? "AI: " + line : "    " + line);
        first = false;
    }
    if (lines.empty())
        lines.push_back("AI: ");
    return lines;
}

inline std::chrono::steady_clock::time_point steady_now() {
    return std::chrono::steady_clock::now();
}
} // namespace mini_agent::tui
