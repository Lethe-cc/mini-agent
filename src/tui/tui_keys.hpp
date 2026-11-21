#pragma once
// TUI main-loop key dispatch (019 P4 / §8). Template over the run_loop
// context. LoopCtl maps the original loop's control flow: Exit == `goto
// done` (run_loop runs the cleanup), Render == fall through to the frame
// section (Next is reserved for outer-loop `continue` translations).
// Continues inside the input byte loop stay plain `continue` (next byte),
// per the §8 hazard note — this file was translated manually.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>
#include "tui_input.hpp"
#include "tui_slash.hpp"
#include "tui_screen.hpp"
#include "mini_agent/tui.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/tools.hpp"

namespace mini_agent::tui {

enum class LoopCtl { Next, Exit, Render };

template <class Ctx>
LoopCtl dispatch_input_impl(Ctx& S, const char* buf, ssize_t n) {
    auto handle_slash = [&](std::string_view line) -> bool {
        return handle_slash_impl(S, line);
    };
    auto hist_up = [&] { hist_up_impl(S); };
    auto hist_down = [&] { hist_down_impl(S); };
    auto mark_edited = [&] { mark_edited_impl(S); };
    auto insert_sidebar_selection = [&]() -> bool { return insert_sidebar_selection_impl(S); };
    for (ssize_t i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(buf[i]);
            if (S.approval_pending()) {
                // Approval mode owns the keyboard (008 5.4): only
                // y/n/a/? act; Esc and Ctrl-C deny the tool AND keep
                // the existing interrupt escalation (abort_requested),
                // so the turn stops cleanly instead of hanging.
                if (c == 'y' || c == 'Y' || c == 'n' || c == 'N' || c == 'a' || c == 'A' ||
                    c == '?') {
                    S.answer_approval(c);
                    continue;
                }
                if (c == 0x03 || c == 0x1b) {
                    S.answer_approval('n');
                    S.abort_requested = true;
                    continue;
                }
                continue; // held: swallowed without touching composer
            }
            if (c == 0x03) { // Ctrl-C
                if (S.preview_pane.open) {
                    S.close_preview_pane(); // 018: modal close, no escalation
                } else if (S.streaming || S.compact_running) {
                    if (S.abort_requested.load()) {
                        // already interrupted, waiting on worker: quit
                        return LoopCtl::Exit;
                    }
                    S.abort_requested = true; // interrupt; redirect per 5.6
                } else {
                    const auto t = steady_now();
                    if (t - S.last_ctrl_c < std::chrono::seconds(2)) {
                        return LoopCtl::Exit;
                    }
                    S.last_ctrl_c = t;
                    S.composer.clear();
                    S.cursor = 0;
                    S.screen.commit("[hint] press Ctrl-C again to quit, or type a new prompt");
                }
            } else if (c == 0x04) { // Ctrl-D
                return LoopCtl::Exit;
            } else if (c == 0x0C) { // Ctrl-L
                S.screen.clear_live();
            } else if (c == 0x06 && S.show_sidebar) { // Ctrl-F: sidebar focus (v4.14)
                S.close_preview_pane();               // 018: modal switch closes the pane
                S.sidebar_focus = !S.sidebar_focus;
                if (S.sidebar_focus) {
                    S.sidebar_sel = 0;
                    // land on the first selectable row
                    for (std::size_t i = 0; i < S.sidebar_result.paths.size(); ++i) {
                        if (!S.sidebar_result.paths[i].empty()) {
                            S.sidebar_sel = i;
                            break;
                        }
                    }
                    // v5: make the preview key discoverable on entry.
                    S.screen.commit("[hint] sidebar: ↑/↓ · Enter=enter · i=insert · "
                                  "p=preview · Esc=leave");
                }

            } else if (c == 0x1B) { // ESC: edit sequences or interrupt
                const ssize_t esc = i;
                ssize_t j = i + 1;
                if (j < n && (buf[j] == '[' || buf[j] == 'O')) {
                    // Full CSI/SS3 sequence (fixes plain-text '[' +
                    // letter leaking into the composer when arrows were
                    // typed). Consume and dispatch it.
                    ++j;
                    while (j < n && !(buf[j] >= 0x40 && buf[j] <= 0x7E))
                        ++j;
                    if (j >= n) {
                        i = n; // partial sequence at chunk edge: swallow
                        continue;
                    }
                    const unsigned char fin = static_cast<unsigned char>(buf[j]);
                    i = j;
                    if (!S.secret_input) { // masked key entry: no cursor UI
                        // v4.14: while the sidebar owns focus, Up/Down
                        // are MODAL: consumed here even when the scan
                        // has no rows yet (falling through to history
                        // recall could auto-send a stale message).
                        const bool nav = S.sidebar_focus;
                        // 018: while the pane is open, navigation keys
                        // scroll it (modal, same precedence as sidebar).
                        const bool pane = S.preview_pane.open;
                        auto scroll_by = [&](long long delta) {
                            const std::size_t vis =
                                pane_visible_rows(static_cast<std::size_t>(term_rows()));
                            const std::size_t cur = S.preview_pane.scroll;
                            const std::size_t next =
                                delta < 0 ? (cur > static_cast<std::size_t>(-delta)
                                                 ? cur - static_cast<std::size_t>(-delta)
                                                 : 0)
                                          : cur + static_cast<std::size_t>(delta);
                            S.preview_pane.scroll =
                                pane_scroll_clamp(next, S.preview_pane.lines.size(), vis);
                        };
                        auto move_sel = [&](int delta) {
                            const auto& paths = S.sidebar_result.paths;
                            if (paths.empty())
                                return;
                            std::size_t cur = S.sidebar_sel;
                            if (cur == static_cast<std::size_t>(-1)) {
                                // No selection (p preview clears it): enter
                                // from the nearest end, not SIZE_MAX wrap.
                                cur = delta > 0 ? 0 : paths.size() - 1;
                                if (!paths[cur].empty()) {
                                    S.sidebar_sel = cur;
                                    return;
                                }
                            }
                            for (std::size_t step = 0; step < paths.size(); ++step) {
                                cur = (cur + paths.size() + static_cast<std::size_t>(delta)) %
                                      paths.size();
                                if (!paths[cur].empty()) {
                                    S.sidebar_sel = cur;
                                    return;
                                }
                            }
                        };
                        switch (fin) {
                        case 'A':
                            if (pane)
                                scroll_by(-1);
                            else if (nav)
                                move_sel(-1);
                            else
                                hist_up();
                            break;
                        case 'B':
                            if (pane)
                                scroll_by(+1);
                            else if (nav)
                                move_sel(+1);
                            else
                                hist_down();
                            break;
                        case 'C':
                            S.cursor = utf8_next(S.composer, S.cursor);
                            break;
                        case 'D':
                            S.cursor = utf8_prev(S.composer, S.cursor);
                            break;
                        case 'H':
                            if (pane)
                                S.preview_pane.scroll = 0;
                            else
                                S.cursor = 0;
                            break;
                        case 'F':
                            if (pane)
                                S.preview_pane.scroll = pane_scroll_clamp(
                                    S.preview_pane.lines.size(), S.preview_pane.lines.size(),
                                    pane_visible_rows(static_cast<std::size_t>(term_rows())));
                            else
                                S.cursor = S.composer.size();
                            break;
                        case 'M':
                        case 'm': {
                            // 018: SGR mouse (\x1b[<btn;x;y M/m). Only the
                            // wheel (64/65) acts, and only while the pane is
                            // open; all other mouse events are swallowed so
                            // they never leak into the composer.
                            const std::string_view mp(buf + esc + 2,
                                                      static_cast<std::size_t>(j - esc - 2));
                            if (pane && !mp.empty() && mp.front() == '<') {
                                const auto semi = mp.find(';');
                                const long long btn = std::strtoll(
                                    std::string(mp.substr(1, semi == std::string_view::npos
                                                                 ? std::string_view::npos
                                                                 : semi - 1))
                                        .c_str(),
                                    nullptr, 10);
                                if (btn == 64)
                                    scroll_by(-3);
                                else if (btn == 65)
                                    scroll_by(+3);
                            }
                            break;
                        }
                        case '~': {
                            const std::string_view params(
                                buf + esc + 2, static_cast<std::size_t>(j - esc - 2));
                            if (params == "5" || params == "6") {
                                if (pane) {
                                    const long long page =
                                        static_cast<long long>(pane_visible_rows(
                                            static_cast<std::size_t>(term_rows())));
                                    scroll_by(params == "5" ? -page : page);
                                }
                            } else if (params == "3") {
                                if (utf8_erase_at(S.composer, S.cursor))
                                    mark_edited();
                            } else if (params == "1") {
                                S.cursor = 0;
                            } else if (params == "4") {
                                S.cursor = S.composer.size();
                            }
                            break;
                        }
                        default:
                            break; // swallow other CSI (paste marks, F-keys)
                        }
                    }
                } else if (S.preview_pane.open) {
                    S.close_preview_pane(); // 018: lone Esc closes the pane
                } else if (S.sidebar_focus) {
                    // v4.14: lone ESC leaves the sidebar focus first
                    // (before the interrupt chain -- focus is modal UI,
                    // same precedence as the menus below).
                    S.sidebar_focus = false;
                } else if (S.streaming || S.compact_running) {
                    // Lone ESC interrupts the in-flight request (design 5.6
                    // rule 5 "esc-to-interrupt", 2026-11-02 maintainer
                    // request): same effect as the first Ctrl-C, without
                    // the double-press-to-quit escalation. 015: a running
                    // /compact is interruptible the same way.
                    S.abort_requested = true;
                } else if (S.reasoning_menu_open) {
                    S.reasoning_menu_open = false;
                    S.reasoning_choices.clear();
                    S.screen.commit("[cancel] reasoning menu dismissed; /reasoning re-opens");
                } else if (S.model_menu_open) {
                    // Lone ESC while the model list is up dismisses it
                    // (nothing else to interrupt); the fetched list stays
                    // in the scrollback and /model <name> still works.
                    S.model_menu_open = false;
                    S.screen.commit("[cancel] model menu dismissed; /models re-lists");
                }
            } else if (c == '\r' || c == '\n') {
                S.close_preview_pane(); // 018: Enter closes, then normal semantics
                if (S.sidebar_focus) {
                    // Modal Enter: insert the highlighted path, or just
                    // leave focus when the scan has nothing selectable
                    // yet -- either way the composer never sees this
                    // Enter (no accidental send of a half-built line).
                    if (S.sidebar_sel < S.sidebar_result.paths.size() &&
                        !S.sidebar_result.paths[S.sidebar_sel].empty()) {
                        const std::string& rawp = S.sidebar_result.paths[S.sidebar_sel];
                        std::error_code dec;
                        if (std::filesystem::is_directory(rawp, dec)) {
                            // 018: Enter on a dir / "." / ".." navigates and
                            // keeps focus so the user can keep browsing.
                            S.enter_sidebar_dir(rawp);
                        } else {
                            insert_sidebar_selection(); // file: insert + leave focus
                        }
                    } else {
                        S.sidebar_focus = false;
                    }
                } else if (S.secret_input) {
                    S.secret_input = false;
                    if (!S.composer.empty()) {
                        // v7: `api_key:<name>` pins the profile chosen at
                        // /login time (there is no unnamed route anymore).
                        bool ok = true;
                        if (S.secret_field.rfind("api_key:", 0) == 0) {
                            const std::string name = S.secret_field.substr(8);
                            if (auto* profile = S.config.find_provider(name)) {
                                profile->api_key = S.composer;
                                S.auth_probes[name] = AuthProbe{};
                                if (name == S.config.current.name) {
                                    S.auth = AuthProbe{}; // stale the moment it changes
                                    S.config.refresh_current();
                                }
                            } else {
                                ok = false;
                                S.screen.commit("[error] provider '" + name +
                                              "' no longer exists; key not saved");
                            }
                        }
                        if (ok) {
                            S.screen.commit("[ok] api_key updated");
                            S.save_and_report(true);
                            S.maybe_autofetch_models();
                        }
                    } else {
                        S.screen.commit("[cancel] input cancelled");
                    }
                    S.composer.clear();
                    S.cursor = 0;
                } else if (!S.composer.empty()) {
                    std::string line;
                    line.swap(S.composer);
                    S.cursor = 0;
                    // Model menu: a number (or an exact id) selects. Any
                    // other input dismisses the menu and falls through
                    // unchanged, so /model <name> and ordinary questions
                    // keep working while the list is on screen.
                    if (S.reasoning_menu_open) {
                        S.reasoning_menu_open = false;
                        // Same numbered-choice helper as the model menu.
                        if (const auto pick = model_choice(S.reasoning_choices, line)) {
                            S.active_profile_for_edit()->reasoning_effort = *pick;
                            S.config.refresh_current(); // v7: per-profile
                            S.save_and_report();
                            S.logger->event(
                                "reasoning_select",
                                {{"level", S.config.current.reasoning_effort}, {"source", "menu"}});
                            S.screen.commit("reasoning_effort=" + S.config.current.reasoning_effort);
                            line.clear();
                        }
                    }
                    if (S.model_menu_open) {
                        S.model_menu_open = false;
                        if (const auto pick = model_choice(S.model_choices, line)) {
                            // v6: the label's index resolves the owning
                            // profile; select_model handles the switch.
                            const auto at =
                                std::find(S.model_choices.begin(), S.model_choices.end(), *pick);
                            const std::size_t idx =
                                static_cast<std::size_t>(at - S.model_choices.begin());
                            if (idx < S.model_choice_targets.size()) {
                                const auto target = S.model_choice_targets[idx];
                                S.select_model(target.first, target.second, "menu");
                            } else {
                                S.select_model("", *pick, "menu");
                            }
                            line.clear();
                        }
                    }
                    if (line.empty()) {
                        // menu selection consumed the input
                    } else {
                        if (line.front() != '/') // slash lines: Tab menu recalls those
                            S.history.push_back(line);
                        S.hist_pos = S.history.size();
                        S.history_draft.clear();
                        if (line == "/exit")
                            return LoopCtl::Exit;
                        if (S.streaming || S.compact_running) {
                            // Pi-style steering (design 007 section 5.6
                            // v1.x candidate 1): Enter while streaming
                            // queues the input; it is sent after the
                            // current turn commits (or is interrupted).
                            // 015: a running compact queues the same way.
                            S.pending.push_back(std::move(line));
                        } else if (!handle_slash(line)) {
                            if (!S.config.complete()) {
                                S.screen.commit("[error] LLM not configured ·" +
                                              config_next_hint(S.config));
                            } else {
                                S.start_turn(line);
                            }
                        }
                    }
                }
            } else if (c == '\t') {
                if (const auto completed = slash_tab_complete(S.composer, S.tab_state, S.session_ids,
                                                              S.provider_name_list(),
                                                              S.model_choices)) {
                    S.composer = *completed;
                    S.cursor = S.composer.size();
                    mark_edited();
                }
            } else if (c == 0x7F || c == 0x08) {
                if (utf8_erase_before(S.composer, S.cursor))
                    mark_edited();
            } else if (c >= 0x20) {
                // v5 (design 011): while the sidebar owns focus, `p`
                // previews the highlighted file into the transcript
                // (no model turn, focus stays) AND clears the selection
                // (2026-11-15 maintainer request: a previewed file must
                // not stay picked, so a stray Enter cannot insert its
                // path). Up/Down re-selects; any other printable char
                // goes to the composer as usual.
                if (S.sidebar_focus && c == 'p') {
                    if (S.sidebar_sel < S.sidebar_result.paths.size() &&
                        !S.sidebar_result.paths[S.sidebar_sel].empty()) {
                        // 018: resolve against the sidebar's own scan root
                        // (repo root for absolute changed entries) so the
                        // listed file always previews; only clear/announce on
                        // success.
                        const std::string& sp = S.sidebar_result.paths[S.sidebar_sel];
                        std::error_code dec;
                        if (std::filesystem::is_directory(sp, dec)) {
                            S.enter_sidebar_dir(sp); // p on a dir enters it too
                        } else {
                            // 018: resolve against the repo root for repo
                            // entries, else the browsed dir; only clear the
                            // selection / announce when it actually opened.
                            const std::string& rr = S.sidebar_result.repo_root;
                            const std::string proot =
                                (!rr.empty() && sp.rfind(rr + "/", 0) == 0)
                                    ? rr
                                    : S.sidebar_result.browse;
                            if (S.open_preview_pane(sp, proot)) {
                                S.sidebar_sel = static_cast<std::size_t>(-1);
                                S.screen.commit("[sidebar] selection cleared · pane open · "
                                              "Esc closes, Ctrl-F re-focuses");
                            }
                        }
                    } else {
                        S.screen.commit("[hint] no sidebar row selected · Up/Down selects, "
                                      "p previews");
                    }
                } else if (S.sidebar_focus && c == 'i') {
                    // 018: insert the highlighted path (file OR dir) for the
                    // model; Enter now navigates into dirs, so `i` is the
                    // explicit "give this path to the model" key.
                    insert_sidebar_selection();
                } else {
                    utf8_insert(S.composer, S.cursor, std::string_view(&buf[i], 1));
                    mark_edited();
                }
            }
            }
    return LoopCtl::Render;
}

} // namespace mini_agent::tui
