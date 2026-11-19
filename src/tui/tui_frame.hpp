#pragma once
// TUI idle-frame render (019 P4). Template over run_loop context; no
// control flow (pure render of the live region).
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include <pwd.h>
#include <unistd.h>
#include "mini_agent/tui.hpp"
#include "mini_agent/tui_bridge.hpp"
#include "tui_preview.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/md.hpp"
namespace mini_agent::tui {
template<class Ctx> void render_frame_impl(Ctx& S) {
            const auto cols = static_cast<std::size_t>(term_columns());
            S.drain_tool_events();                 // [tool] rows land in S.history as they arrive
            std::vector<std::string> extra_rows; // approval block when S.pending
            const std::string tail = S.streaming ? S.bridge.deltas_snapshot() : std::string{};
            // v4.11 turn panel: while S.streaming, the extra-rows slot shows
            // the process (thinking marker + tool rows incl. the running
            // call). Approval rows keep priority when a decision is S.pending
            // (the S.pending call IS the process at that moment).
            if (S.streaming && !S.approval_pending()) {
                std::vector<std::string> panel_tools = S.panel_tool_rows;
                if (const auto running = S.bridge.running_tool(); !running.empty())
                    panel_tools.push_back(running);
                const bool thinking = S.reasoning_live.load(std::memory_order_relaxed);
                const auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(steady_now() - S.turn_start)
                        .count();
                extra_rows = turn_panel_rows(colors_enabled(), thinking, secs, panel_tools, 5);
            }
            std::string composer_display;
            if (S.secret_input) {
                composer_display.assign(S.composer.size(), '*');
                if (composer_display.empty())
                    composer_display = "(type key, Enter to confirm)";
            } else if (S.composer.empty()) {
                // Caret-only cell keeps the reverse-video block visible at
                // an empty prompt; the queue hint trails it.
                composer_display = "\x1b[7m \x1b[27m";
                if (S.streaming)
                    composer_display += "(streaming… type to queue)";
            } else {
                // Reverse-video block on the codepoint under the caret (or a
                // blank at line end). md::wrap treats the SGR as zero width,
                // so row accounting (5.6 rule 2) is unaffected.
                composer_display = S.composer.substr(0, S.cursor);
                composer_display += "\x1b[7m";
                if (S.cursor < S.composer.size()) {
                    const std::size_t nxt = utf8_next(S.composer, S.cursor);
                    composer_display.append(S.composer, S.cursor, nxt - S.cursor);
                    composer_display += "\x1b[27m";
                    composer_display.append(S.composer, nxt, std::string::npos);
                } else {
                    composer_display += ' ';
                }
                composer_display += "\x1b[27m";
            }
            // Masked input passes an empty raw S.composer: no slash hint.
            const std::string raw_for_hint = S.secret_input ? std::string() : S.composer;
            // v4.13: while the S.composer sits in the /S.session arg tier
            // ("/session …"), keep the id snapshot current so Tab and the
            // hint list see sessions saved by this or another process.
            if (!S.session_ids_stale && starts_with(raw_for_hint, "/session "))
                S.session_ids_stale = true;
            if (S.session_ids_stale)
                S.refresh_session_ids();
            // Prompt marker = provider connection state, derived purely from
            // (S.config, last probe, listing-in-flight): an idle tick yields the
            // same text, so render() keeps its zero-work input cache.
            const std::string prompt =
                prompt_marker(connection_state(S.config, S.auth, S.models_running));
            // Status-row colouring (2026-11-04 readability request): the
            // identity prefix dims, state words carry the hue (listing/
            // S.streaming cyan, idle dim, auto-tools red, AWAITING APPROVAL
            // bold yellow). SGR is zero-width for md::clip, and the clip
            // keeps any active span (it re-emits escapes while cutting).
            const bool sc = colors_enabled();
            // Status segment order: state words FIRST, identity and usage
            // last. The v4.16 merged footer clips the status to whatever
            // the sysinfo leaves (v7.2 guarantees >=24 cols on wide
            // terminals); with the identity prefix first, the S.streaming/
            // queued/auto-tools/approval markers were exactly the part
            // that got clipped away. State is the part that must survive;
            // model/S.session also live in the banner.
            std::string status;
            if (S.models_running) {
                status += tint(sc, "36", "listing models...");
            } else if (S.model_menu_open) {
                status += tint(sc, "36", "pick a model (Esc cancels)");
            }
            if (S.compact_running) {
                status +=
                    tint(sc, "36", (status.empty() ? "" : " · ") + std::string("compacting..."));
            }
            if (S.streaming) {
                // Interrupt keys stay documented in /help and the S.composer
                // hint; elapsed seconds live in the turn panel header. The
                // status word must fit the footer clip budget (v7.2: >=24
                // cols on an 80-col terminal) WITH the queue depth attached.
                status += tint(sc, "36", (status.empty() ? "" : " · ") + std::string("streaming"));
                if (!S.pending.empty()) {
                    status += tint(sc, "36", " · queued " + std::to_string(S.pending.size()));
                }
            } else if (status.empty()) {
                status += tint(sc, "2", "idle");
            }
            if (S.approval_policy == "off" && !S.tool_registry->empty())
                status += tint(sc, "1;31",
                               (status.empty() ? "" : " · ") +
                                   std::string("auto-tools")); // persistent danger marker (008 5.4)
            if (S.approval_pending()) {
                status += tint(sc, "1;33",
                               (status.empty() ? "" : " · ") + std::string("AWAITING APPROVAL"));
                // v4.17: after 90 s of the 120 s auto-deny budget, show the
                // remaining seconds. The line changes once per second, which
                // naturally busts render()'s input cache (no per-frame
                // repaint otherwise).
                if (const long long waited_s = S.bridge.approval_waited_s();
                    waited_s >= kApprovalWarnAtS) {
                    const long long left = std::max<long long>(0, kApprovalTimeoutS - waited_s);
                    status += tint(sc, "1;31", " · auto-deny in " + std::to_string(left) + "s");
                }
            }
            // v4.15: the id follows /S.session switches (like the banner) and
            // carries the temp tag; was S.options.session_id (launch snapshot).
            status += tint(sc, "2",
                           (status.empty() ? "" : " · ") + std::string("model=") +
                               (S.config.current.model.empty() ? std::string("(not set)")
                                                             : S.config.current.model) +
                               " · session=" + S.active_session_id +
                               (S.temp_mode ? " (temp)" : std::string()));
            // v4: usage tail (ctx/out/reasoning) last: least critical under
            // the footer clip, and /context still reports it in full.
            status += status_usage_segments(sc, S.context_reading());
            if (S.approval_pending()) {
                const ToolCall snapshot = S.bridge.approval_call();
                const std::string snapshot_preview = S.bridge.approval_preview();
                extra_rows = approval_rows(snapshot.name, snapshot.arguments_json,
                                           S.approval_expanded, snapshot_preview);
            }
            // 018: the preview pane owns the extra-rows slot when idle
            // (S.streaming/approval cannot coexist with it; menus close it).
            // Scroll/resize change extra_rows, busting the render cache.
            if (S.preview_pane.open && !S.streaming && !S.approval_pending()) {
                if (S.preview_pane.rendered_cols != S.cols_now()) {
                    S.preview_pane.lines = render_preview_lines(colors_enabled(), S.preview_pane.body,
                                                              S.preview_pane.path, S.cols_now());
                    S.preview_pane.rendered_cols = S.cols_now();
                }
                const std::size_t vis = pane_visible_rows(static_cast<std::size_t>(term_rows()));
                S.preview_pane.scroll =
                    pane_scroll_clamp(S.preview_pane.scroll, S.preview_pane.lines.size(), vis);
                extra_rows.clear();
                extra_rows.push_back(pane_header(S.preview_pane.path, S.preview_pane.scroll, vis,
                                                 S.preview_pane.lines.size(), S.cols_now()));
                const std::size_t end =
                    std::min(S.preview_pane.lines.size(), S.preview_pane.scroll + vis);
                for (std::size_t k = S.preview_pane.scroll; k < end; ++k)
                    extra_rows.push_back(S.preview_pane.lines[k]);
            }
            // v4.12: bottom system-info row (user@host · cwd · clock).
            // Gathered per frame: the clock is the only fast-changing part
            // and row-level diffing keeps the repaint cost at one row per
            // second. Home abbreviated to ~ like the Pi footer; workspace
            // stamp (S.session boundary) shown when set, else process cwd.
            std::string sysinfo;
            if (S.show_sysinfo) {
                std::string user;
                if (const char* u = ::getenv("USER"))
                    user = u;
                else if (const auto* pw = ::getpwuid(::getuid()))
                    user = pw->pw_name;
                else
                    user = "?";
                char hostbuf[256] = {0};
                if (::gethostname(hostbuf, sizeof(hostbuf) - 1) != 0)
                    std::snprintf(hostbuf, sizeof(hostbuf), "?");
                std::string cwd = S.config.workspace.empty()
                                      ? std::filesystem::current_path().string()
                                      : S.config.workspace;
                if (const char* home = ::getenv("HOME"); home && *home) {
                    const std::string h = home;
                    if (cwd.size() >= h.size() && cwd.compare(0, h.size(), h) == 0)
                        cwd = "~" + cwd.substr(h.size());
                }
                char clockbuf[16];
                const auto wall = std::chrono::system_clock::now();
                const auto secs = std::chrono::system_clock::to_time_t(wall);
                std::tm tm{};
                ::localtime_r(&secs, &tm);
                std::snprintf(clockbuf, sizeof(clockbuf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
                              tm.tm_sec);
                sysinfo = sysinfo_line(sc, user, hostbuf, cwd, clockbuf);
            }
            std::vector<std::string> sidebar_rows;
            if (S.show_sidebar) {
                std::vector<std::string> raw = S.sidebar_result.rows;
                if (!S.sidebar_result.error.empty())
                    raw = {"sidebar: " + S.sidebar_result.error};
                else if (S.sidebar_running)
                    raw.push_back("(scanning...)");
                sidebar_rows.push_back(tint(sc, "1", "workspace"));
                for (const auto& row : raw)
                    sidebar_rows.push_back(tint(sc, "2", sanitize_display(row)));
            }
            const std::size_t sel = S.sidebar_focus && S.sidebar_sel < S.sidebar_result.rows.size()
                                        ? S.sidebar_sel + 1 // +1: header row shifts every entry
                                        : static_cast<std::size_t>(-1);
            S.screen.render(tail, S.last_user, raw_for_hint, composer_display, prompt, status,
                          S.streaming, cols, extra_rows, sysinfo, S.session_ids, sidebar_rows, sel,
                          S.provider_name_list(), S.model_choices);
}
} // namespace mini_agent::tui
