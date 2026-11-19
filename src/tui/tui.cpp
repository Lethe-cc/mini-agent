// Self-drawn minimal TUI (ADR 003) implementing the frame model and
// interaction contract of design 007 section 5.6.
//
// Rendering (5.6 rule 2; supersedes 5.4's "alternate screen" note): inline
// main-screen rendering. Committed history prints once and lives in the
// terminal's native scrollback; only a small "live region" (response tail,
// composer, status) is redrawn in place. No alternate screen, no full diff.
#include "mini_agent/tui.hpp"

#include "mini_agent/context.hpp"
#include "mini_agent/highlight.hpp"
#include "mini_agent/tui_bridge.hpp"

#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/models_dev.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tui_preview.hpp"
#include "tui_screen.hpp"
#include "tui_sidebar.hpp"
#include "tui_turn.hpp"
#include "tui_slash.hpp"
#include "tui_input.hpp"
#include "tui_frame.hpp"
#include "tui_workers.hpp"

namespace mini_agent::tui {

int run_loop(AppConfig& config, Session& session, SessionStore& store, const TuiOptions& options) {
    RawMode raw(STDIN_FILENO);
    std::signal(SIGINT, SIG_IGN); // Ctrl-C must arrive as byte 0x03, not a signal

    LiveScreen screen;
    // 019 §8: TurnState owns the shared turn/approval control state; aliases
    // keep the existing main-thread code byte-identical.
    TurnState S;
    auto& bridge = S.bridge;
    auto& abort_requested = S.abort_requested;
    auto& turn_done = S.turn_done;
    auto& result = S.result;
    auto& reasoning_live = S.reasoning_live;
    auto& approval_abort = S.approval_abort;
    auto& session_allowed_tools = S.session_allowed_tools;
    auto& session_allowed_mutex = S.session_allowed_mutex;
    auto& config_snapshot = S.config_snapshot;
    auto& max_tool_rounds = S.max_tool_rounds;
    auto& tool_registry = S.tool_registry;
    auto& approval_policy = S.approval_policy;
    auto cols_now = [] {
        const int c = term_columns();
        return c > 0 ? static_cast<std::size_t>(c) : 80u;
    };
    bool exiting{false};
    std::thread worker;
    std::string composer;
    std::size_t cursor = 0; // caret byte offset in composer (codepoint boundary)
    std::string last_user;
    std::vector<std::string> pending; // Pi-style Enter queue (5.6 v1.x #1)
    // Single-line ↑/↓ history (2026-11-01 arrow-key request): hist_pos ==
    // history.size() means the draft slot (what the user is typing now).
    // Seeded from the restored session so --session resumes recall too.
    std::vector<std::string> history;
    for (const auto& m : session.chat.messages())
        if (m.role == Role::User && !m.content.empty() && m.content.front() != '/')
            history.push_back(m.content);
    std::size_t hist_pos = history.size();
    std::string history_draft;
    auto make_logger = [&config] {
        return std::make_unique<Logger>(
            Logger::resolve_path(config).value_or(std::filesystem::path{}));
    };
    auto logger = make_logger();
    // v4.9: /session switches the live session in-process, so the banner/
    // status id is a mutable copy of the launch id, not TuiOptions itself.
    std::string active_session_id = options.session_id;
    // v4.15: temp mode is mutable too. `/session new` from a temp session
    // keeps it temp; `/session <id>` resumes a saved session and ends it
    // (the target is a normal, persisted session).
    bool temp_mode = options.temp;
    logger->event("session_open", {{"mode", "tui"},
                                   {"id", active_session_id},
                                   {"model", config.current.model},
                                   {"log", logger->enabled() ? "on" : "off"}});
    // v7 (design 013 §5.2): one metadata event when a v1 file was migrated
    // in memory; the file itself is rewritten on the next save.
    if (config.migrated_from_v1)
        logger->event("config_migrate", {{"from", "1"}, {"to", "2"},
                                         {"profiles", std::to_string(config.providers.size())}});
    bool secret_input = false; // /login: mask composer echo
    std::string secret_field;  // target key when secret_input
    // Model discovery (2026-11-02 request): a separate short-lived worker
    // queries GET {base_url}/models so the frame loop never blocks on it.
    // The result is read only after join(), mirroring `result`/`turn_done`.
    std::thread models_worker;
    std::atomic<bool> models_done{false};
    bool models_running = false;
    ModelsResult models_result;
    AuthProbe auth; // ACTIVE profile's verdict, shown by /config too
    // v6: per-profile verdicts survive provider switches ("" = unnamed).
    // `auth` always mirrors the active profile's map entry.
    std::map<std::string, AuthProbe> auth_probes;
    std::string models_fingerprint; // every target's name+url+key, '\n'-joined (not a hash)
    std::chrono::steady_clock::time_point models_start;
    bool model_menu_open = false;
    std::vector<std::string> model_choices; // display labels backing the open menu
    // v6: selection target for each model_choices row (provider "" = keep
    // the active profile). Parallel to model_choices, same lifecycle.
    std::vector<std::pair<std::string, std::string>> model_choice_targets;
    // v4.3 (design 010 §13): one-shot context-window probe worker, same
    // join-then-read discipline as models_worker (screen/logger stay on
    // the main thread).
    std::thread probe_worker;
    std::atomic<bool> probe_done{false};
    bool probe_running = false;
    ProbeResult probe_result;
    // 015 (design 015 §4C): /compact summary worker, same join-then-read
    // discipline as probe_worker. The summary replaces session.chat ONLY
    // on the main thread after a successful join; failure/abort leaves
    // the history untouched.
    std::thread compact_worker;
    std::atomic<bool> compact_done{false};
    bool compact_running = false;
    CompactResult compact_result;
    // v4.2 (design 010 §11): capability metadata from the last successful
    // /models fetch (main-thread only; the worker fills models_result.caps).
    std::vector<ModelCapabilities> model_caps;
    bool reasoning_menu_open = false;
    std::vector<std::string> reasoning_choices; // efforts backing the open menu
    int tab_state = 0;
    // v4.13 (maintainer request): session-id snapshot for /session Tab
    // completion + prefix resolution. Readdir is cheap but not free, so it
    // refreshes on edges only: entering/leaving the /session arg tier, and a
    // completed turn (the save that creates the file). Also part of render()'s
    // input cache, so a refresh legitimately yields a new frame.
    std::vector<std::string> session_ids;
    bool session_ids_stale = true;
    auto refresh_session_ids = [&store, &session_ids, &session_ids_stale] {
        std::string err;
        session_ids = store.list(&err); // empty on error: hints just get shorter
        session_ids_stale = false;
    };
    bool streaming = false;
    std::size_t last_prompt_chars = 0;
    // v4 status-bar state: usage readings folded from successful turns
    // (main-thread only, written after worker join). When no provider ever
    // reported usage, the ctx reading falls back to a character estimate
    // of the outgoing chat, recomputed lazily at turn boundaries.
    long long status_completion_tokens = 0;
    // 017 (design 017 §2.1): provider-billed anchor (baseline = chat size
    // minus the final assistant reply at commit), overflow synthesis and
    // the one-shot pressure hint. All cleared by reset_usage_readings.
    std::optional<UsageAnchor> usage_anchor;
    long long exceeded_tokens = 0;
    bool pressure_hint_consumed = false;
    // v4.2: reasoning ACTUALLY observed in responses (status preference).
    bool status_reasoning_seen = false;
    std::string status_reasoning_actual;
    // v6 (design 012): profile names for /login and /provider completion.
    auto provider_name_list = [&] {
        std::vector<std::string> names;
        names.reserve(config.providers.size());
        for (const auto& p : config.providers)
            names.push_back(p.name);
        return names;
    };
    // v4.2: cached capability lookup for the current model (empty when the
    // gateway exposed no metadata or the model is not in the list).
    auto caps_for_model = [&]() -> ModelCapabilities {
        for (const auto& cap : model_caps)
            if (cap.id == config.current.model)
                return cap;
        return {};
    };
    auto effective_window = [&]() -> long long {
        long long window = config.current.context_window;
        // v4.2.1: config stays authoritative, but an unset window falls
        // back to the gateway's advertised context_length (cached caps).
        if (window <= 0)
            window = caps_for_model().context_length;
        // v4.7 (design 010 §18): pi-style static catalog last.
        if (window <= 0) {
            if (const auto limits = lookup_models_dev(config.current.base_url, config.current.model))
                window = limits->context;
        }
        return window;
    };
    auto context_reading = [&]() -> StatusInputs {
        StatusInputs inputs;
        inputs.context_window = effective_window();
        inputs.reasoning_effort = config.current.reasoning_effort;
        inputs.completion_tokens = status_completion_tokens;
        inputs.reasoning_seen = status_reasoning_seen;
        inputs.reasoning_actual = status_reasoning_actual;
        // 017: THE single reading (anchor+tail / full estimate / exceeded
        // synthesis) -- status, /context and the hint all consume this.
        const auto reading =
            mini_agent::context_reading(session.chat, usage_anchor, exceeded_tokens);
        inputs.context_tokens = reading.used;
        inputs.context_estimated = reading.estimated;
        inputs.context_exceeded = reading.exceeded;
        inputs.breakdown = reading.breakdown;
        return inputs;
    };
    // 017 (design 017 §2.1): one commit line per threshold crossing; the
    // hint never auto-compacts (015 decision). Re-arms below threshold.
    auto maybe_pressure_hint = [&] {
        const auto inputs = context_reading();
        const long long threshold = pressure_threshold(inputs.context_window);
        if (threshold <= 0)
            return;
        if (inputs.context_tokens >= threshold) {
            if (!pressure_hint_consumed) {
                pressure_hint_consumed = true;
                screen.commit("[hint] context at " +
                              std::to_string(inputs.context_tokens * 100 / inputs.context_window) +
                              "% of window · consider /compact (history stays until you run it)");
            }
        } else {
            pressure_hint_consumed = false;
        }
    };
    // v4: switching models invalidates every usage reading (the window and
    // all prior counts belong to the old route).
    auto reset_usage_readings = [&] {
        status_completion_tokens = 0;
        usage_anchor.reset(); // 017: prefix changed, the anchor is void
        exceeded_tokens = 0;
        pressure_hint_consumed = false;
        status_reasoning_seen = false; // v4.2: evidence belongs to the old model
        status_reasoning_actual.clear();
        reasoning_menu_open = false;
        reasoning_choices.clear();
    };
    // v6 (design 012 §5.4): provider switching. `auth` always mirrors the
    // active profile's map entry; per-profile verdicts survive the switch.
    // The capability cache is invalidated (it belongs to the old route) and
    // every usage reading resets, exactly like a cross-route model change.
    auto switch_active_profile = [&] {
        auth = auth_probes[config.current.name]; // default-constructed on first sight
        model_caps.clear();
        reset_usage_readings();
    };
    auto turn_start = steady_now();
    auto last_ctrl_c = std::chrono::steady_clock::time_point{};

    const auto banner_model = config.current.model.empty() ? std::string("(no model)") : config.current.model;
    // v4.15: tag the id with how it was chosen (temp | resumed | new).
    const char* session_tag = temp_mode ? "temp" : (options.resumed ? "resumed" : "new");
    // 0.8.0: the banner carries the build version (MINI_AGENT_VERSION is
    // defined for agent_core by CMake; fallback keeps foreign builds sane).
#ifndef MINI_AGENT_VERSION
#define MINI_AGENT_VERSION "0.0.0-dev"
#endif
#ifndef MINI_AGENT_COMMIT
#define MINI_AGENT_COMMIT "unknown"
#endif
#ifndef MINI_AGENT_BUILD_TIME
#define MINI_AGENT_BUILD_TIME "unknown"
#endif
    screen.commit("mini-agent v" MINI_AGENT_VERSION " · model=" + banner_model +
                  " · session=" + active_session_id + " (" + session_tag + ")");
    if (!config.complete()) {
        // Names only the next missing step: the three commands save
        // independently, so they may be supplied one per sitting.
        screen.commit("[warn] LLM not configured · " + config_next_hint(config) +
                      " (env vars also work)");
    } else {
        // Fully configured from a previous run: the stored key has not been
        // checked against the provider yet in this process.
        screen.commit(auth_status_line(auth));
    }
    screen.commit("Enter sends; Enter while streaming queues; Ctrl-C/Esc interrupt; "
                  "Ctrl-C twice quits; /help; Tab completes");

    // v2 tool layer (design 008 section 5.4): registry, approval bridge.
    std::string tool_note;
    S.tool_registry = build_tool_registry(config, &tool_note);
    // v4.11 turn panel (design 010 §21): live process rows during a turn.
    // `running_tool` is set by on_tool_start (worker thread) and cleared
    // by on_tool -- both through TurnBridge's tool queue; `reasoning_live`
    // is set by the SSE parser the first time a reasoning delta arrives.
    bool show_sysinfo = true; // v4.12: /sysinfo on|off toggle
    // v4.13: right sidebar (workspace tree + changed files). Data comes
    // from a short-lived worker (fs walk + `git status --porcelain`): the
    // frame loop must not block, same join-then-read discipline as the
    // models fetch. Refresh triggers: /sidebar on, turn completion, /cwd,
    // /session switch.
    bool show_sidebar = false;
    std::thread sidebar_worker;
    std::atomic<bool> sidebar_done{false};
    bool sidebar_running = false;
    bool sidebar_stale = false; // refresh requested while a scan ran
    SidebarResult sidebar_result;
    // v4.14: sidebar focus mode (Ctrl-F): arrow keys walk selectable rows,
    // Enter inserts the file path into the composer, Esc leaves. Index is
    // into rows/paths; navigation skips non-selectable entries.
    bool sidebar_focus = false;
    std::size_t sidebar_sel = 0;
    // 018: single-level browser current dir (empty = workspace root). Enter on
    // a directory / "." / ".." navigates; the git "changed:" section is unaffected.
    std::string sidebar_browse;
    // 018 (design 018): preview pane -- a closable, scrollable view rendered
    // in the live region (extra_rows); the transcript only gets open/closed
    // marker lines. Main-thread only, same discipline as screen/logger.
    struct PreviewPaneState {
        bool open = false;
        std::string path;
        std::string body;
        std::vector<std::string> lines;
        std::size_t scroll = 0;
        std::size_t rendered_cols = 0;
    } preview_pane;
    // 018: SGR mouse reporting, enabled only while the pane is open so normal
    // terminal text-selection is unaffected the rest of the time. 1000=button
    // events, 1006=SGR encoding; wheel up/down arrive as button 64/65.
    auto set_preview_mouse = [](bool on) {
        const char* seq = on ? "\x1b[?1000h\x1b[?1006h" : "\x1b[?1000l\x1b[?1006l";
        ::fwrite(seq, 1, std::strlen(seq), stdout);
        ::fflush(stdout);
    };
    auto close_preview_pane = [&] {
        if (!preview_pane.open)
            return;
        preview_pane.open = false;
        set_preview_mouse(false);
        screen.commit(tint(colors_enabled(), "2", "[preview] closed: " + preview_pane.path));
        logger->event("preview_close", {{"path", preview_pane.path}});
    };
    // Completed tool rows for THIS turn, kept for the panel even after
    // drain_tool_events commits them to history (the panel is the turn's
    // process view; history scrolls away).
    std::vector<std::string> panel_tool_rows;
    bool approval_expanded = false;
    // Allowlist names are snapshotted per turn like the rest of config;
    // /tools and slash handlers rebuild both the registry and this copy.
    S.config_snapshot = config;
    S.max_tool_rounds = config.max_tool_rounds;
    bool tools_active = false; // any tool round ran in the current turn
    if (!tool_note.empty())
        screen.commit("[info] " + tool_note + " · lazy start on first call · /tools");
    S.approval_policy = // mutable via /approve while worker idle
        config.tool_approval.empty() ? std::string("ask") : config.tool_approval;
    if (approval_policy == "off" && !tool_registry->empty()) {
        screen.commit("[warn] tool_approval=off: tools run WITHOUT asking");
        logger->event("tools", {{"status", "auto"}, {"approval", "off"}});
    }
    logger->event("tools", {{"status", "ready"},
                            {"approval", approval_policy},
                            {"servers", std::to_string(config.mcp_servers.size())},
                            {"builtin", tool_registry->empty() ? "no" : "yes"}});

    auto drain_tool_events = [&] {
        // 014: provider retries are metadata-only log events (no history
        // row -- the streaming state word already covers the wait).
        for (const auto& retry : bridge.drain_retry_events()) {
            logger->event("provider_retry", {{"attempt", std::to_string(retry.attempt)},
                                             {"status", std::to_string(retry.http_status)},
                                             {"wait_ms", std::to_string(retry.wait_ms)}});
        }
        for (const auto& round : bridge.drain_tool_events()) {
            screen.commit(tool_activity_line(round.tool_name, round.arguments_json, round.executed,
                                             round.is_error, round.elapsed_ms, round.result_bytes));
            // v5 (design 011): echo the applied diff / preview body right
            // after the activity row -- edit -> diff -> effect in one
            // transcript flow.
            for (const auto& row :
                 result_preview_rows(round.tool_name, round.result_preview,
                                     round.preview_is_markdown, round.preview_lang, cols_now()))
                screen.commit(row);
            // v4.11: the panel keeps the whole turn's rows (history scrolls).
            panel_tool_rows.push_back(tool_activity_line(round.tool_name, round.arguments_json,
                                                         round.executed, round.is_error,
                                                         round.elapsed_ms, round.result_bytes));
            tools_active = true;
        }
    };

    auto approval_pending = [&] {
        return bridge.approval_pending();
    };
    // Answer the pending approval with the UI key semantics (008 5.4):
    // y allow once, n deny, a allow-name for the session, Esc/Ctrl-C deny.
    auto answer_approval = [&](int key) {
        ToolCall call = bridge.approval_call(); // snapshot under the slot mutex
        int answer = 2;
        bool handled = true;
        switch (key) {
        case 'y':
        case 'Y':
            answer = 1;
            break;
        case 'a':
        case 'A':
            answer = 3;
            break;
        case 'n':
        case 'N':
            answer = 2;
            break;
        case '?':
            approval_expanded = !approval_expanded;
            handled = false;
            break;
        default:
            handled = false; // other keys are held (swallowed) while pending
            break;
        }
        if (!handled)
            return;
        // Session-allow is per tool NAME, argument-blind: say so out loud
        // at the moment of consent, loudest for run_bash (any shell command).
        if (answer == 3) {
            if (call.name == "run_bash") {
                screen.commit("[warn] session-allow: EVERY run_bash command now runs "
                              "unapproved (this session)");
            } else {
                screen.commit("[info] session-allow: every " + call.name +
                              " call runs unapproved this session (any arguments)");
            }
        }
        bridge.answer_approval(answer);
        logger->event("tool_approve",
                      {{"tool", call.name},
                       {"decision", answer == 1 ? "allow" : (answer == 3 ? "session" : "deny")}});
    };

    auto start_turn = [&](const std::string& text) {
        close_preview_pane(); // 018: streaming needs the live region
        if (worker.joinable())
            worker.join();
        config_snapshot = config; // refresh after join, before worker start
        last_user = text;
        bridge.reset_turn();
        abort_requested = false;
        turn_done = false;
        result = TurnResult{};
        reasoning_live = false; // v4.11 panel evidence belongs to the new turn
        panel_tool_rows.clear();
        approval_abort = false;
        approval_expanded = false;
        tools_active = false;
        turn_start = steady_now();
        streaming = true;
        last_prompt_chars = text.size();
        Message user;
        user.role = Role::User;
        user.content = text;
        // Snapshot config for the worker; the provider must be constructed
        // INSIDE the thread (start_turn returns before the worker runs).
        // Intentional snapshot: config mutates on the main thread while the
        // worker runs (/login, /model); the copy is the thread boundary.
        const AppConfig turn_config = config; // NOLINT(performance-unnecessary-copy-initialization)
        // 014: retry events log through the CURRENT logger. The worker
        // parks them in the bridge (data, not a logger pointer): /log may
        // reassign `logger` mid-turn, and only the main thread touches it.
        worker = std::thread([&, turn_config, user_msg = std::move(user)]() mutable {
            run_turn_worker(S, turn_config, session.chat, user_msg.content);
        });
    };

    auto save_and_report = [&](bool show_progress = false) {
        std::string err;
        if (save_config_file(config, &err)) {
            screen.commit("[ok] config saved (api_key stored in config file, 0600)");
            // Batched entry: after each independently-saved piece, name only
            // what is still missing so the user knows this step landed and
            // what to supply next (possibly in a later session).
            if (show_progress)
                screen.commit(config_next_hint(config));
        } else {
            screen.commit("[warn] config save failed: " + err);
        }
    };

    // Query GET {base_url}/models off-thread (frame loop must not block).
    // v6 (design 012 §5.3): ONE aggregate job per fetch. Every complete
    // profile (url+key set) gets its own sequential GET -- partial profiles
    // are skipped until /login completes them -- plus the unnamed top-level
    // values when they are set and not already covered by a profile (the
    // env-override case). Per-profile failure never blocks the others.
    // Logs only metadata (status/count/elapsed): api_keys never reach the
    // log line, the menu, or any ProviderError text.
    auto start_sidebar = [&] {
        if (!show_sidebar)
            return;
        if (sidebar_running) {
            // A scan is in flight (git status on a big repo can take
            // seconds): NEVER join on the main thread -- that would stall
            // the frame loop (v4.14 regression: cascading smoke failures
            // from a blocked poll tick). Mark stale; the completion
            // handler re-fires once the in-flight scan lands.
            sidebar_stale = true;
            return;
        }
        if (sidebar_worker.joinable())
            sidebar_worker.join(); // previous scan is done: cheap join
        sidebar_result = SidebarResult{};
        sidebar_done = false;
        sidebar_running = true;
        const std::string root =
            config.workspace.empty() ? std::filesystem::current_path().string() : config.workspace;
        const std::string browse = sidebar_browse.empty() ? root : sidebar_browse;
        sidebar_worker = std::thread([&, root, browse] {
            sidebar_result = scan_sidebar(root, browse);
            sidebar_done = true;
        });
    };

    // 018: browser navigation -- enter a directory (or "."/"..") and rescan,
    // keeping sidebar focus so the user can keep walking. Files are handled by
    // the caller (insert / preview), not here.
    auto enter_sidebar_dir = [&](const std::string& dir) {
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(dir, ec);
        if (ec || !std::filesystem::is_directory(canon, ec))
            return;
        sidebar_browse = canon.string();
        sidebar_sel = 0;
        start_sidebar();
        screen.commit("[sidebar] cd " + sanitize_display(sidebar_browse));
    };

    auto start_models_fetch = [&](bool force) {
        if (models_running)
            return;
        // Fetch targets: profile name -> (base_url, api_key). With no
        // profiles (env-only state) fetch the current values once, unnamed
        // (design 013 §5.4); env overrides on top of profiles are annotated
        // in the menu, not fetched as a separate route.
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> targets;
        std::string fingerprint;
        for (const auto& profile : config.providers) {
            fingerprint += profile.name + '\n' + profile.base_url + '\n' + profile.api_key + '\n';
            if (!profile.base_url.empty() && !profile.api_key.empty())
                targets.emplace_back(profile.name,
                                     std::make_pair(profile.base_url, profile.api_key));
        }
        // v7 (design 013 §5.4): with no profiles (env-only state) fetch the
        // current values once. Under env override the effective route may
        // differ from every stored profile, so fetch it too -- named after
        // current.name so the auth verdict lands on the effective profile.
        if (!config.current.base_url.empty() && !config.current.api_key.empty() &&
            (config.providers.empty() || config.env_override_active)) {
            fingerprint += '\n' + config.current.base_url + '\n' + config.current.api_key + '\n';
            bool covered = false;
            for (auto& [name, creds] : targets) {
                if (name == config.current.name) {
                    // Same profile, env values win over its stored ones.
                    creds = std::make_pair(config.current.base_url, config.current.api_key);
                    covered = true;
                }
            }
            if (!covered)
                targets.emplace_back(config.current.name,
                                     std::make_pair(config.current.base_url, config.current.api_key));
        }
        if (targets.empty())
            return;
        if (!force && fingerprint == models_fingerprint)
            return;
        if (models_worker.joinable())
            models_worker.join();
        models_running = true;
        models_done = false;
        models_result = ModelsResult{};
        model_menu_open = false;
        models_fingerprint = fingerprint;
        models_start = steady_now();
        logger->event("models_list",
                      {{"status", "start"},
                       {"base_url", Logger::redact_url(config.current.base_url)},
                       {"providers", std::to_string(targets.size())}});
        const AppConfig snapshot = config; // thread boundary (like start_turn)
        // The worker computes only the result and sets the flag: `logger` and
        // `screen` are main-thread state (/log reassigns logger), so the
        // completion events and the menu render happen on the main thread.
        models_worker = std::thread([&, snapshot, targets] {
            models_result = fetch_models(snapshot, targets);
            models_done = true;
        });
    };

    // Called after /provider or /login mutates config: auto-discover once
    // at least one target has both halves known (the user's stated request).
    auto maybe_autofetch_models = [&] {
        bool any_ready = !config.current.base_url.empty() && !config.current.api_key.empty();
        for (const auto& profile : config.providers)
            if (!profile.base_url.empty() && !profile.api_key.empty())
                any_ready = true;
        if (any_ready) {
            screen.commit("[info] listing models from provider...");
            start_models_fetch(false);
        }
    };

    // v5 (design 011): show a workspace file to the human without
    // involving the model. Markdown renders; everything else shows verbatim.
    // Read-only, bounded to the session workspace -- the same rule the file
    // tools enforce (ADR 007). Shared by /preview and the sidebar `p` key.
    // 018 (design 018): loader extracted from 011's preview_workspace_file;
    // validation semantics (workspace bounds, regular file, binary reject)
    // unchanged. The body is kept so the pane can re-render on resize.
    auto open_preview_pane = [&](const std::string& path_raw,
                                 const std::string& root_override = "") -> bool {
        // 018 gate: the live region belongs to streaming/compact/approval.
        if (streaming || compact_running || approval_pending()) {
            screen.commit("[info] preview needs an idle turn · wait or Ctrl-C");
            return false;
        }
        std::string body, err;
        if (!load_preview_body(config.workspace, path_raw, &body, &err, root_override)) {
            screen.commit("[error] " + err);
            return false;
        }
        close_preview_pane(); // replacing an open pane leaves a closed marker
        preview_pane.open = true;
        preview_pane.path = path_raw;
        preview_pane.body = std::move(body);
        preview_pane.lines =
            render_preview_lines(colors_enabled(), preview_pane.body, path_raw, cols_now());
        preview_pane.scroll = 0;
        preview_pane.rendered_cols = cols_now();
        sidebar_focus = false; // 018: the pane takes over from sidebar focus
        set_preview_mouse(true);
        screen.commit("[preview] " + sanitize_display(path_raw) + " · " +
                      std::to_string(preview_pane.lines.size()) + " lines · Esc closes");
        logger->event("preview_open",
                      {{"path", path_raw}, {"lines", std::to_string(preview_pane.lines.size())}});
        return true;
    };
    // v7 (design 013 §5.4): any persisting command auto-creates the default
    // profile when none exists, keeping first-run friction at zero. The
    // caller edits the profile and then refreshes current.
    auto ensure_default_profile = [&] {
        if (!config.providers.empty())
            return;
        ProviderProfile profile;
        profile.name = "default";
        config.providers.push_back(std::move(profile));
        config.active_provider = "default";
    };
    // Persisting edits target the active profile (auto-default first); the
    // caller refreshes current afterwards.
    auto active_profile_for_edit = [&] {
        ensure_default_profile();
        return config.find_provider(config.active_provider);
    };
    // v6 (design 012 §5.2): one selection path for /model and the numbered
    // menu. Cross-profile picks activate the profile first (atomic
    // url+key+model switch). v7: the model id is written into the active
    // profile and current is re-derived; env overrides keep winning on
    // every derivation until the vars are unset and the process restarts.
    auto select_model = [&](const std::string& provider, const std::string& id,
                            const char* source) {
        const bool switch_profile = !provider.empty() && provider != config.active_provider;
        if (switch_profile) {
            if (!config.activate_provider(provider)) {
                screen.commit("[error] unknown provider '" + provider + "'");
                return;
            }
            switch_active_profile();
            logger->event("provider_activate", {{"provider", provider}, {"source", source}});
        }
        ensure_default_profile();
        if (auto* active = config.find_provider(config.active_provider))
            active->model = id;
        config.refresh_current();
        session.meta.model = config.current.model;
        session.meta.provider =
            config.current.name.empty()
                ? std::optional<std::string>()
                : std::optional<std::string>(config.current.name);
        reset_usage_readings(); // v4: window/counts belong to the old model
        if (source == std::string_view("menu"))
            save_and_report(); // menu keeps the legacy quiet save
        else
            save_and_report(true);
        logger->event("model_select",
                      {{"model", config.current.model},
                       {"source", source},
                       {"provider",
                        config.current.name.empty() ? "(none)" : config.current.name}});
        screen.commit("model=" + (config.current.name.empty()
                                      ? config.current.model
                                      : config.current.name + "/" + config.current.model));
        if (config.env_override_active)
            screen.commit("[warn] MINI_AGENT_* env overrides are active: the profile is "
                          "saved, but this process keeps the env endpoint/model until "
                          "the vars are unset and mini-agent restarts");
    };
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wchanges-meaning"
#endif
    struct SlashCtx {
        decltype(screen)& screen;
        decltype(config)& config;
        decltype(session)& session;
        decltype(store)& store;
        decltype(options)& options;
        decltype(cols_now)& cols_now;
        decltype(composer)& composer;
        decltype(cursor)& cursor;
        decltype(last_user)& last_user;
        decltype(pending)& pending;
        decltype(history)& history;
        decltype(hist_pos)& hist_pos;
        decltype(history_draft)& history_draft;
        decltype(make_logger)& make_logger;
        decltype(logger)& logger;
        decltype(active_session_id)& active_session_id;
        decltype(temp_mode)& temp_mode;
        decltype(secret_input)& secret_input;
        decltype(secret_field)& secret_field;
        decltype(models_running)& models_running;
        decltype(models_done)& models_done;
        decltype(models_result)& models_result;
        decltype(models_worker)& models_worker;
        decltype(model_menu_open)& model_menu_open;
        decltype(model_choices)& model_choices;
        decltype(model_choice_targets)& model_choice_targets;
        decltype(models_fingerprint)& models_fingerprint;
        decltype(models_start)& models_start;
        decltype(model_caps)& model_caps;
        decltype(reasoning_menu_open)& reasoning_menu_open;
        decltype(reasoning_choices)& reasoning_choices;
        decltype(probe_running)& probe_running;
        decltype(probe_done)& probe_done;
        decltype(probe_result)& probe_result;
        decltype(probe_worker)& probe_worker;
        decltype(compact_running)& compact_running;
        decltype(compact_done)& compact_done;
        decltype(compact_result)& compact_result;
        decltype(compact_worker)& compact_worker;
        decltype(auth)& auth;
        decltype(auth_probes)& auth_probes;
        decltype(session_ids)& session_ids;
        decltype(session_ids_stale)& session_ids_stale;
        decltype(refresh_session_ids)& refresh_session_ids;
        decltype(streaming)& streaming;
        decltype(last_prompt_chars)& last_prompt_chars;
        decltype(pressure_hint_consumed)& pressure_hint_consumed;
        decltype(status_reasoning_seen)& status_reasoning_seen;
        decltype(provider_name_list)& provider_name_list;
        decltype(caps_for_model)& caps_for_model;
        decltype(effective_window)& effective_window;
        decltype(context_reading)& context_reading;
        decltype(maybe_pressure_hint)& maybe_pressure_hint;
        decltype(reset_usage_readings)& reset_usage_readings;
        decltype(switch_active_profile)& switch_active_profile;
        decltype(turn_start)& turn_start;
        decltype(last_ctrl_c)& last_ctrl_c;
        decltype(show_sysinfo)& show_sysinfo;
        decltype(show_sidebar)& show_sidebar;
        decltype(sidebar_running)& sidebar_running;
        decltype(sidebar_stale)& sidebar_stale;
        decltype(sidebar_focus)& sidebar_focus;
        decltype(sidebar_sel)& sidebar_sel;
        decltype(sidebar_result)& sidebar_result;
        decltype(sidebar_done)& sidebar_done;
        decltype(sidebar_browse)& sidebar_browse;
        decltype(sidebar_worker)& sidebar_worker;
        decltype(preview_pane)& preview_pane;
        decltype(set_preview_mouse)& set_preview_mouse;
        decltype(close_preview_pane)& close_preview_pane;
        decltype(approval_expanded)& approval_expanded;
        decltype(tools_active)& tools_active;
        decltype(panel_tool_rows)& panel_tool_rows;
        decltype(drain_tool_events)& drain_tool_events;
        decltype(approval_pending)& approval_pending;
        decltype(answer_approval)& answer_approval;
        decltype(start_turn)& start_turn;
        decltype(save_and_report)& save_and_report;
        decltype(start_sidebar)& start_sidebar;
        decltype(enter_sidebar_dir)& enter_sidebar_dir;
        decltype(start_models_fetch)& start_models_fetch;
        decltype(maybe_autofetch_models)& maybe_autofetch_models;
        decltype(open_preview_pane)& open_preview_pane;
        decltype(ensure_default_profile)& ensure_default_profile;
        decltype(active_profile_for_edit)& active_profile_for_edit;
        decltype(select_model)& select_model;
        decltype(bridge)& bridge;
        decltype(abort_requested)& abort_requested;
        decltype(turn_done)& turn_done;
        decltype(result)& result;
        decltype(reasoning_live)& reasoning_live;
        decltype(approval_abort)& approval_abort;
        decltype(session_allowed_tools)& session_allowed_tools;
        decltype(session_allowed_mutex)& session_allowed_mutex;
        decltype(config_snapshot)& config_snapshot;
        decltype(max_tool_rounds)& max_tool_rounds;
        decltype(tool_registry)& tool_registry;
        decltype(approval_policy)& approval_policy;
        decltype(worker)& worker;
        decltype(exiting)& exiting;
    } slash_ctx{screen, config, session, store, options, cols_now, composer, cursor, last_user, pending, history, hist_pos, history_draft, make_logger, logger, active_session_id, temp_mode, secret_input, secret_field, models_running, models_done, models_result, models_worker, model_menu_open, model_choices, model_choice_targets, models_fingerprint, models_start, model_caps, reasoning_menu_open, reasoning_choices, probe_running, probe_done, probe_result, probe_worker, compact_running, compact_done, compact_result, compact_worker, auth, auth_probes, session_ids, session_ids_stale, refresh_session_ids, streaming, last_prompt_chars, pressure_hint_consumed, status_reasoning_seen, provider_name_list, caps_for_model, effective_window, context_reading, maybe_pressure_hint, reset_usage_readings, switch_active_profile, turn_start, last_ctrl_c, show_sysinfo, show_sidebar, sidebar_running, sidebar_stale, sidebar_focus, sidebar_sel, sidebar_result, sidebar_done, sidebar_browse, sidebar_worker, preview_pane, set_preview_mouse, close_preview_pane, approval_expanded, tools_active, panel_tool_rows, drain_tool_events, approval_pending, answer_approval, start_turn, save_and_report, start_sidebar, enter_sidebar_dir, start_models_fetch, maybe_autofetch_models, open_preview_pane, ensure_default_profile, active_profile_for_edit, select_model, bridge, abort_requested, turn_done, result, reasoning_live, approval_abort, session_allowed_tools, session_allowed_mutex, config_snapshot, max_tool_rounds, tool_registry, approval_policy, worker, exiting};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    auto handle_slash = [&](std::string_view line) -> bool {
        return handle_slash_impl(slash_ctx, line);
    };

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wchanges-meaning"
#endif
    struct InputCtx {
        decltype(hist_pos)& hist_pos;
        decltype(history)& history;
        decltype(history_draft)& history_draft;
        decltype(composer)& composer;
        decltype(cursor)& cursor;
        decltype(sidebar_sel)& sidebar_sel;
        decltype(sidebar_result)& sidebar_result;
        decltype(config)& config;
        decltype(screen)& screen;
        decltype(sidebar_focus)& sidebar_focus;
    } input_ctx{hist_pos, history, history_draft, composer, cursor, sidebar_sel, sidebar_result, config, screen, sidebar_focus};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    auto hist_up = [&] { hist_up_impl(input_ctx); };
    auto hist_down = [&] { hist_down_impl(input_ctx); };
    auto mark_edited = [&] { mark_edited_impl(input_ctx); };
    auto insert_sidebar_selection = [&]() -> bool { return insert_sidebar_selection_impl(input_ctx); };

    for (;;) {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(options.frame_ms));
        if (ready > 0 && (pfd.revents & POLLIN)) {
            char buf[256];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                break; // EOF: Ctrl-D or closed stdin
            }
            for (ssize_t i = 0; i < n; ++i) {
                const unsigned char c = static_cast<unsigned char>(buf[i]);
                if (approval_pending()) {
                    // Approval mode owns the keyboard (008 5.4): only
                    // y/n/a/? act; Esc and Ctrl-C deny the tool AND keep
                    // the existing interrupt escalation (abort_requested),
                    // so the turn stops cleanly instead of hanging.
                    if (c == 'y' || c == 'Y' || c == 'n' || c == 'N' || c == 'a' || c == 'A' ||
                        c == '?') {
                        answer_approval(c);
                        continue;
                    }
                    if (c == 0x03 || c == 0x1b) {
                        answer_approval('n');
                        abort_requested = true;
                        continue;
                    }
                    continue; // held: swallowed without touching composer
                }
                if (c == 0x03) { // Ctrl-C
                    if (preview_pane.open) {
                        close_preview_pane(); // 018: modal close, no escalation
                    } else if (streaming || compact_running) {
                        if (abort_requested.load()) {
                            // already interrupted, waiting on worker: quit
                            goto done;
                        }
                        abort_requested = true; // interrupt; redirect per 5.6
                    } else {
                        const auto t = steady_now();
                        if (t - last_ctrl_c < std::chrono::seconds(2)) {
                            goto done;
                        }
                        last_ctrl_c = t;
                        composer.clear();
                        cursor = 0;
                        screen.commit("[hint] press Ctrl-C again to quit, or type a new prompt");
                    }
                } else if (c == 0x04) { // Ctrl-D
                    goto done;
                } else if (c == 0x0C) { // Ctrl-L
                    screen.clear_live();
                } else if (c == 0x06 && show_sidebar) { // Ctrl-F: sidebar focus (v4.14)
                    close_preview_pane();               // 018: modal switch closes the pane
                    sidebar_focus = !sidebar_focus;
                    if (sidebar_focus) {
                        sidebar_sel = 0;
                        // land on the first selectable row
                        for (std::size_t i = 0; i < sidebar_result.paths.size(); ++i) {
                            if (!sidebar_result.paths[i].empty()) {
                                sidebar_sel = i;
                                break;
                            }
                        }
                        // v5: make the preview key discoverable on entry.
                        screen.commit("[hint] sidebar: ↑/↓ · Enter=enter · i=insert · "
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
                        if (!secret_input) { // masked key entry: no cursor UI
                            // v4.14: while the sidebar owns focus, Up/Down
                            // are MODAL: consumed here even when the scan
                            // has no rows yet (falling through to history
                            // recall could auto-send a stale message).
                            const bool nav = sidebar_focus;
                            // 018: while the pane is open, navigation keys
                            // scroll it (modal, same precedence as sidebar).
                            const bool pane = preview_pane.open;
                            auto scroll_by = [&](long long delta) {
                                const std::size_t vis =
                                    pane_visible_rows(static_cast<std::size_t>(term_rows()));
                                const std::size_t cur = preview_pane.scroll;
                                const std::size_t next =
                                    delta < 0 ? (cur > static_cast<std::size_t>(-delta)
                                                     ? cur - static_cast<std::size_t>(-delta)
                                                     : 0)
                                              : cur + static_cast<std::size_t>(delta);
                                preview_pane.scroll =
                                    pane_scroll_clamp(next, preview_pane.lines.size(), vis);
                            };
                            auto move_sel = [&](int delta) {
                                const auto& paths = sidebar_result.paths;
                                if (paths.empty())
                                    return;
                                std::size_t cur = sidebar_sel;
                                if (cur == static_cast<std::size_t>(-1)) {
                                    // No selection (p preview clears it): enter
                                    // from the nearest end, not SIZE_MAX wrap.
                                    cur = delta > 0 ? 0 : paths.size() - 1;
                                    if (!paths[cur].empty()) {
                                        sidebar_sel = cur;
                                        return;
                                    }
                                }
                                for (std::size_t step = 0; step < paths.size(); ++step) {
                                    cur = (cur + paths.size() + static_cast<std::size_t>(delta)) %
                                          paths.size();
                                    if (!paths[cur].empty()) {
                                        sidebar_sel = cur;
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
                                cursor = utf8_next(composer, cursor);
                                break;
                            case 'D':
                                cursor = utf8_prev(composer, cursor);
                                break;
                            case 'H':
                                if (pane)
                                    preview_pane.scroll = 0;
                                else
                                    cursor = 0;
                                break;
                            case 'F':
                                if (pane)
                                    preview_pane.scroll = pane_scroll_clamp(
                                        preview_pane.lines.size(), preview_pane.lines.size(),
                                        pane_visible_rows(static_cast<std::size_t>(term_rows())));
                                else
                                    cursor = composer.size();
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
                                    if (utf8_erase_at(composer, cursor))
                                        mark_edited();
                                } else if (params == "1") {
                                    cursor = 0;
                                } else if (params == "4") {
                                    cursor = composer.size();
                                }
                                break;
                            }
                            default:
                                break; // swallow other CSI (paste marks, F-keys)
                            }
                        }
                    } else if (preview_pane.open) {
                        close_preview_pane(); // 018: lone Esc closes the pane
                    } else if (sidebar_focus) {
                        // v4.14: lone ESC leaves the sidebar focus first
                        // (before the interrupt chain -- focus is modal UI,
                        // same precedence as the menus below).
                        sidebar_focus = false;
                    } else if (streaming || compact_running) {
                        // Lone ESC interrupts the in-flight request (design 5.6
                        // rule 5 "esc-to-interrupt", 2026-11-02 maintainer
                        // request): same effect as the first Ctrl-C, without
                        // the double-press-to-quit escalation. 015: a running
                        // /compact is interruptible the same way.
                        abort_requested = true;
                    } else if (reasoning_menu_open) {
                        reasoning_menu_open = false;
                        reasoning_choices.clear();
                        screen.commit("[cancel] reasoning menu dismissed; /reasoning re-opens");
                    } else if (model_menu_open) {
                        // Lone ESC while the model list is up dismisses it
                        // (nothing else to interrupt); the fetched list stays
                        // in the scrollback and /model <name> still works.
                        model_menu_open = false;
                        screen.commit("[cancel] model menu dismissed; /models re-lists");
                    }
                } else if (c == '\r' || c == '\n') {
                    close_preview_pane(); // 018: Enter closes, then normal semantics
                    if (sidebar_focus) {
                        // Modal Enter: insert the highlighted path, or just
                        // leave focus when the scan has nothing selectable
                        // yet -- either way the composer never sees this
                        // Enter (no accidental send of a half-built line).
                        if (sidebar_sel < sidebar_result.paths.size() &&
                            !sidebar_result.paths[sidebar_sel].empty()) {
                            const std::string& rawp = sidebar_result.paths[sidebar_sel];
                            std::error_code dec;
                            if (std::filesystem::is_directory(rawp, dec)) {
                                // 018: Enter on a dir / "." / ".." navigates and
                                // keeps focus so the user can keep browsing.
                                enter_sidebar_dir(rawp);
                            } else {
                                insert_sidebar_selection(); // file: insert + leave focus
                            }
                        } else {
                            sidebar_focus = false;
                        }
                    } else if (secret_input) {
                        secret_input = false;
                        if (!composer.empty()) {
                            // v7: `api_key:<name>` pins the profile chosen at
                            // /login time (there is no unnamed route anymore).
                            bool ok = true;
                            if (secret_field.rfind("api_key:", 0) == 0) {
                                const std::string name = secret_field.substr(8);
                                if (auto* profile = config.find_provider(name)) {
                                    profile->api_key = composer;
                                    auth_probes[name] = AuthProbe{};
                                    if (name == config.current.name) {
                                        auth = AuthProbe{}; // stale the moment it changes
                                        config.refresh_current();
                                    }
                                } else {
                                    ok = false;
                                    screen.commit("[error] provider '" + name +
                                                  "' no longer exists; key not saved");
                                }
                            }
                            if (ok) {
                                screen.commit("[ok] api_key updated");
                                save_and_report(true);
                                maybe_autofetch_models();
                            }
                        } else {
                            screen.commit("[cancel] input cancelled");
                        }
                        composer.clear();
                        cursor = 0;
                    } else if (!composer.empty()) {
                        std::string line;
                        line.swap(composer);
                        cursor = 0;
                        // Model menu: a number (or an exact id) selects. Any
                        // other input dismisses the menu and falls through
                        // unchanged, so /model <name> and ordinary questions
                        // keep working while the list is on screen.
                        if (reasoning_menu_open) {
                            reasoning_menu_open = false;
                            // Same numbered-choice helper as the model menu.
                            if (const auto pick = model_choice(reasoning_choices, line)) {
                                active_profile_for_edit()->reasoning_effort = *pick;
                                config.refresh_current(); // v7: per-profile
                                save_and_report();
                                logger->event(
                                    "reasoning_select",
                                    {{"level", config.current.reasoning_effort}, {"source", "menu"}});
                                screen.commit("reasoning_effort=" + config.current.reasoning_effort);
                                line.clear();
                            }
                        }
                        if (model_menu_open) {
                            model_menu_open = false;
                            if (const auto pick = model_choice(model_choices, line)) {
                                // v6: the label's index resolves the owning
                                // profile; select_model handles the switch.
                                const auto at =
                                    std::find(model_choices.begin(), model_choices.end(), *pick);
                                const std::size_t idx =
                                    static_cast<std::size_t>(at - model_choices.begin());
                                if (idx < model_choice_targets.size()) {
                                    const auto target = model_choice_targets[idx];
                                    select_model(target.first, target.second, "menu");
                                } else {
                                    select_model("", *pick, "menu");
                                }
                                line.clear();
                            }
                        }
                        if (line.empty()) {
                            // menu selection consumed the input
                        } else {
                            if (line.front() != '/') // slash lines: Tab menu recalls those
                                history.push_back(line);
                            hist_pos = history.size();
                            history_draft.clear();
                            if (line == "/exit")
                                goto done;
                            if (streaming || compact_running) {
                                // Pi-style steering (design 007 section 5.6
                                // v1.x candidate 1): Enter while streaming
                                // queues the input; it is sent after the
                                // current turn commits (or is interrupted).
                                // 015: a running compact queues the same way.
                                pending.push_back(std::move(line));
                            } else if (!handle_slash(line)) {
                                if (!config.complete()) {
                                    screen.commit("[error] LLM not configured ·" +
                                                  config_next_hint(config));
                                } else {
                                    start_turn(line);
                                }
                            }
                        }
                    }
                } else if (c == '\t') {
                    if (const auto completed = slash_tab_complete(composer, tab_state, session_ids,
                                                                  provider_name_list(),
                                                                  model_choices)) {
                        composer = *completed;
                        cursor = composer.size();
                        mark_edited();
                    }
                } else if (c == 0x7F || c == 0x08) {
                    if (utf8_erase_before(composer, cursor))
                        mark_edited();
                } else if (c >= 0x20) {
                    // v5 (design 011): while the sidebar owns focus, `p`
                    // previews the highlighted file into the transcript
                    // (no model turn, focus stays) AND clears the selection
                    // (2026-11-15 maintainer request: a previewed file must
                    // not stay picked, so a stray Enter cannot insert its
                    // path). Up/Down re-selects; any other printable char
                    // goes to the composer as usual.
                    if (sidebar_focus && c == 'p') {
                        if (sidebar_sel < sidebar_result.paths.size() &&
                            !sidebar_result.paths[sidebar_sel].empty()) {
                            // 018: resolve against the sidebar's own scan root
                            // (repo root for absolute changed entries) so the
                            // listed file always previews; only clear/announce on
                            // success.
                            const std::string& sp = sidebar_result.paths[sidebar_sel];
                            std::error_code dec;
                            if (std::filesystem::is_directory(sp, dec)) {
                                enter_sidebar_dir(sp); // p on a dir enters it too
                            } else {
                                // 018: resolve against the repo root for repo
                                // entries, else the browsed dir; only clear the
                                // selection / announce when it actually opened.
                                const std::string& rr = sidebar_result.repo_root;
                                const std::string proot =
                                    (!rr.empty() && sp.rfind(rr + "/", 0) == 0)
                                        ? rr
                                        : sidebar_result.browse;
                                if (open_preview_pane(sp, proot)) {
                                    sidebar_sel = static_cast<std::size_t>(-1);
                                    screen.commit("[sidebar] selection cleared · pane open · "
                                                  "Esc closes, Ctrl-F re-focuses");
                                }
                            }
                        } else {
                            screen.commit("[hint] no sidebar row selected · Up/Down selects, "
                                          "p previews");
                        }
                    } else if (sidebar_focus && c == 'i') {
                        // 018: insert the highlighted path (file OR dir) for the
                        // model; Enter now navigates into dirs, so `i` is the
                        // explicit "give this path to the model" key.
                        insert_sidebar_selection();
                    } else {
                        utf8_insert(composer, cursor, std::string_view(&buf[i], 1));
                        mark_edited();
                    }
                }
            }
        }

        // frame: drain buffer snapshot and render live region (5.6 rule 1)

        // Model-list completion: rendered here on the main thread so the
        // worker never touches screen/logger (both are main-thread state).
        if (models_done.load()) {
            models_done = false;
            models_running = false;
            if (models_worker.joinable())
                models_worker.join();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(steady_now() - models_start)
                    .count();
            // v6 (design 012 §5.3): fold every target's sub-result. The
            // credential probe verdict lands in the per-profile map; menu
            // rows aggregate all successful listings (qualified labels when
            // more than one target answered, bare ids for the legacy single
            // route so the existing `models (N)` contract stays intact).
            const bool aggregate = models_result.parts.size() > 1;
            model_choices.clear();
            model_choice_targets.clear();
            model_caps.clear(); // rebuilt from the ACTIVE target below
            std::vector<std::string> failures;
            bool any_ok = false;
            bool active_probed = false;
            for (auto& part : models_result.parts) {
                std::vector<std::pair<std::string, std::string>> list_fields = {
                    {"provider", part.name.empty() ? "(none)" : part.name},
                    {"elapsed_ms", std::to_string(ms)},
                };
                if (part.ok) {
                    any_ok = true;
                    list_fields.emplace_back("status", "ok");
                    list_fields.emplace_back("count", std::to_string(part.ids.size()));
                    logger->event("models_list", list_fields);
                    for (const auto& id : part.ids) {
                        model_choices.push_back(aggregate && !part.name.empty()
                                                    ? part.name + "/" + id
                                                    : id);
                        model_choice_targets.emplace_back(part.name, id);
                    }
                    if (part.name == config.current.name)
                        model_caps = std::move(part.caps); // v4.2
                    part.caps.clear();
                } else {
                    list_fields.emplace_back("status", "error");
                    list_fields.emplace_back("count", "0");
                    list_fields.emplace_back("error", part.error);
                    logger->event("models_list", list_fields);
                    failures.push_back((part.name.empty() ? std::string("(none)") : part.name) +
                                       ": " + part.error);
                }
                // The request is also the credential probe: a 401 means
                // re-enter /login, a 200 with an unparseable body still
                // proves the key is good. Per-profile verdict (design 012).
                AuthProbe probe;
                probe.attempted = true;
                probe.fetch_ok = part.ok;
                probe.http_status = part.http_status;
                probe.network_error = part.network_error;
                probe.model_count = part.ids.size();
                auth_probes[part.name] = probe;
                if (part.name == config.current.name) {
                    auth = probe;
                    active_probed = true;
                }
                std::vector<std::pair<std::string, std::string>> login_fields = {
                    {"status", std::string(auth_log_token(probe))},
                    {"models", std::to_string(probe.model_count)},
                    {"provider", part.name.empty() ? "(none)" : part.name},
                };
                // Only report a status code when one actually arrived: a
                // success path never raised one, and writing `http_status=0`
                // there would read like a bogus response rather than "not
                // applicable".
                if (probe.http_status != 0)
                    login_fields.emplace_back("http_status", std::to_string(probe.http_status));
                logger->event("login", login_fields);
            }
            close_preview_pane(); // 018: menu takes over the live region
            model_menu_open = !model_choices.empty();
            if (model_menu_open) {
                // `<- current` tags the active combination's label.
                const std::string current_label =
                    aggregate && !config.current.name.empty()
                        ? config.current.name + "/" + config.current.model
                        : config.current.model;
                for (const auto& row : model_menu_rows(model_choices, current_label))
                    screen.commit(row);
            } else if (!any_ok) {
                screen.commit("[error] model list failed: " + models_result.error);
            } else {
                screen.commit("[info] provider listed no models; use /model <name>");
            }
            // Failures never block the successful listings: summarize them
            // at the menu tail, one row per failed profile.
            for (const auto& failure : failures)
                screen.commit("[warn] model list failed for " + failure);
            if (active_probed)
                screen.commit(auth_status_line(auth));
        }

        // 015: compact completion (main-thread apply, same join-then-read
        // discipline as the probe).
        if (compact_done.load()) {
            compact_done = false;
            compact_running = false;
            if (compact_worker.joinable())
                compact_worker.join();
            const std::size_t before = session.chat.messages().size();
            if (compact_result.aborted) {
                logger->event("compact",
                              {{"status", "aborted"}, {"before", std::to_string(before)}});
                screen.commit("[interrupted] compact (history unchanged)");
            } else if (!compact_result.error.empty()) {
                logger->event("compact", {{"status", "error"},
                                          {"before", std::to_string(before)},
                                          {"error", compact_result.error}});
                screen.commit("[error] compact failed: " + compact_result.error +
                              " (history unchanged)");
            } else {
                // Replace the transcript with the leading System prompt (if
                // any) + ONE marked User summary. User role, not Assistant:
                // never fabricate model speech; no tool data, no schema bump.
                std::vector<Message> next;
                if (!session.chat.messages().empty() &&
                    session.chat.messages().front().role == Role::System)
                    next.push_back(session.chat.messages().front());
                Message summary;
                summary.role = Role::User;
                summary.content = "[conversation summary]\n" + compact_result.summary;
                next.push_back(std::move(summary));
                // 017: the retained tail follows the summary verbatim.
                next.insert(next.end(), compact_result.tail.begin(), compact_result.tail.end());
                session.chat.replace(std::move(next));
                std::string err;
                if (!temp_mode && !store.save(session, &err)) {
                    screen.commit("[warn] save failed: " + err);
                    logger->event("session_save", {{"status", "error"}, {"error", err}});
                }
                reset_usage_readings(); // the old counts belong to the old history
                logger->event("compact",
                              {{"status", "ok"},
                               {"before", std::to_string(before)},
                               {"after", std::to_string(session.chat.messages().size())},
                               {"pruned", std::to_string(compact_result.pruned)},
                               {"retained", std::to_string(compact_result.tail.size())},
                               {"nudged", compact_result.nudged ? "1" : "0"},
                               {"summary_chars", std::to_string(compact_result.summary.size())}});
                screen.commit("[ok] compacted " + std::to_string(before) +
                              " messages into a "
                              "summary + " +
                              std::to_string(compact_result.tail.size()) + " retained · " +
                              std::to_string(compact_result.pruned) + " tool outputs pruned" +
                              (compact_result.nudged ? " · nudged" : ""));
                maybe_pressure_hint();
            }
            // Drain the steering queue exactly like a turn commit.
            if (!exiting && !pending.empty()) {
                std::string next = std::move(pending.front());
                pending.erase(pending.begin());
                if (next == "/exit") {
                    goto done;
                }
                if (!handle_slash(next)) {
                    if (config.complete()) {
                        start_turn(next);
                    } else {
                        screen.commit("[error] LLM not configured, dropped queued message ·" +
                                      config_next_hint(config));
                    }
                } else {
                    continue; // slash handled, keep rendering idle frame
                }
            }
        }
        // v4.3: window-probe completion (main-thread rendering).
        if (probe_done.load()) {
            probe_done = false;
            probe_running = false;
            if (probe_worker.joinable())
                probe_worker.join();
            if (!probe_result.error.empty()) {
                logger->event("ctx_probe", {{"status", "error"}, {"error", probe_result.error}});
                screen.commit("[error] probe failed: " + probe_result.error);
            } else if (probe_result.outcome.kind == ProbeLimitKind::Window ||
                       probe_result.outcome.kind == ProbeLimitKind::InputCap) {
                // v4.3.1: only a total window or an input cap may become
                // the ctx denominator (the prompt side of the reading).
                const auto kind = probe_result.outcome.kind;
                const long long window = probe_result.outcome.value;
                // v7: the reading belongs to the probed (effective) profile;
                // env-only state keeps it session-only (no profile to save).
                if (auto* probed = config.find_provider(config.current.name)) {
                    probed->context_window = window;
                    config.refresh_current();
                    save_and_report();
                } else {
                    config.current.context_window = window;
                }
                logger->event("ctx_probe",
                              {{"status", "ok"},
                               {"window", std::to_string(window)},
                               {"kind", kind == ProbeLimitKind::Window ? "window" : "input_cap"}});
                screen.commit("[ok] probed " +
                              std::string(kind == ProbeLimitKind::Window ? "context window: "
                                                                         : "input limit: ") +
                              format_token_count(window) + " (" + std::to_string(window) +
                              " tokens) · saved to config (override: /context <tokens>)");
            } else if (probe_result.outcome.kind == ProbeLimitKind::OutputCap) {
                // The gateway's max_tokens validator fired first: that is
                // the OUTPUT cap, not the window. Show it, save nothing.
                logger->event("ctx_probe", {{"status", "output_cap"},
                                            {"value", std::to_string(probe_result.outcome.value)}});
                screen.commit("[info] gateway reported its OUTPUT cap: " +
                              format_token_count(probe_result.outcome.value) + " (" +
                              std::to_string(probe_result.outcome.value) +
                              " tokens) · not the context window · set the window: "
                              "/context <tokens>");
            } else {
                logger->event("ctx_probe", {{"status", "unknown"}});
                screen.commit("[info] gateway accepted the oversized request without reporting a "
                              "limit (silent clamp) · set the window manually: /context <tokens>");
            }
        }

        // v4.13: sidebar scan completion (main-thread rendering).
        if (sidebar_done.load()) {
            sidebar_done = false;
            sidebar_running = false;
            if (sidebar_worker.joinable())
                sidebar_worker.join();
            if (sidebar_stale) {
                sidebar_stale = false;
                start_sidebar(); // the turn/cwd changed mid-scan: rescan
            }
        }

        if (turn_done.load()) {
            streaming = false;
            if (worker.joinable())
                worker.join();
            drain_tool_events(); // last tool rows commit before the reply
            const auto login_before = auth_log_token(auth);
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(steady_now() - turn_start)
                    .count();
            if (result.ok) {
                // v4: fold this turn's usage into the status-bar readings.
                // prompt side = live context (last round), completion side
                // = session sum. Turns without a usage chunk leave the
                // estimate fallback in play (context_estimated flips).
                if (!result.usage.empty()) {
                    if (result.usage.prompt_tokens > 0 && !session.chat.messages().empty()) {
                        // 017: the billed last request contained everything
                        // except the final assistant reply (run_turn commits
                        // it after the last round) -> anchor baseline.
                        usage_anchor = UsageAnchor{session.chat.messages().size() - 1,
                                                   result.usage.prompt_tokens};
                    }
                    status_completion_tokens += result.usage.completion_tokens;
                }
                exceeded_tokens = 0; // a successful turn clears the synthesis
                // v4.2.1: the reading reflects the LAST successful turn
                // only (no sticky "on"): fold the fresh evidence wholesale,
                // clearing it when this turn showed none.
                status_reasoning_seen = result.reasoning.seen;
                status_reasoning_actual = result.reasoning.effort;
                logger->event("turn", {{"status", "ok"},
                                       {"elapsed_ms", std::to_string(elapsed_ms)},
                                       {"chars_in", std::to_string(last_prompt_chars)},
                                       {"chars_out", std::to_string(result.reply.size())}});
                screen.clear_live();
                screen.commit("You: " + last_user);
                if (::isatty(STDOUT_FILENO)) {
                    for (auto& para : split_ansi_paragraphs(
                             md::render(result.reply, cols_now() > 1 ? cols_now() - 1 : 1))) {
                        screen.commit(para);
                    }
                } else {
                    screen.commit("AI: " + result.reply);
                }
                std::string err;
                if (temp_mode) {
                    // v4.15: temp sessions never touch the store.
                } else if (!store.save(session, &err)) {
                    screen.commit("[warn] save failed: " + err);
                    logger->event("session_save", {{"status", "error"}, {"error", err}});
                }
                start_sidebar(); // v4.13: the turn may have changed files
                maybe_pressure_hint(); // 017: one hint line per crossing
            } else if (result.aborted) {
                logger->event("turn",
                              {{"status", "aborted"}, {"elapsed_ms", std::to_string(elapsed_ms)}});
                screen.clear_live();
                screen.commit("[interrupted] You: " + last_user);
            } else {
                logger->event("turn", {{"status", "error"},
                                       {"elapsed_ms", std::to_string(elapsed_ms)},
                                       {"error", result.error}});
                screen.clear_live();
                screen.commit("You: " + last_user);
                screen.commit("[error] " + result.error);
                // v4: a 400 right after we started sending reasoning_effort
                // is most likely the gateway rejecting the field -- name the
                // probable cause and the fix instead of leaving the raw error.
                if (result.http_status == 400 && !config.current.reasoning_effort.empty())
                    screen.commit("[hint] if this gateway does not support reasoning_effort, "
                                  "clear it with /reasoning off (config value is otherwise "
                                  "kept verbatim)");
                // 015 (design 015 §4B) + 017: overflow-shaped 400 (and not
                // a throttling rejection in overflow wording) -> actionable
                // guidance + synthesized Exceeded reading (Zed discipline).
                if (result.http_status == 400 && !overflow_excluded(result.error) &&
                    context_overflow_message(result.error)) {
                    const long long synthesized = std::max(
                        parse_overflow_tokens(result.error).value_or(0), effective_window());
                    if (synthesized > 0)
                        exceeded_tokens = synthesized;
                    screen.commit("[hint] context window exceeded · /compact summarizes "
                                  "history (recent turns kept verbatim, original backed "
                                  "up), or /session new starts fresh");
                }
                if (tools_active)
                    screen.commit("[note] tools already ran this turn; their side "
                                  "effects were not rolled back");
            }
            // The completion request was itself a credential probe, so fold its
            // outcome into the same verdict the model listing feeds: a working
            // chat must read as connected even on providers that expose no
            // /models endpoint (2026-11-02 report: chat replied, the prompt
            // stayed yellow). Only real evidence counts -- an aborted turn or a
            // local protocol error says nothing about the key and leaves the
            // previous verdict alone.
            if (result.ok) {
                auth.attempted = true;
                auth.fetch_ok = true;
                auth.http_status = 0;
                auth.network_error = false;
            } else if (!result.aborted && (result.http_status != 0 || result.network_error)) {
                auth.attempted = true;
                auth.fetch_ok = false;
                auth.http_status = result.http_status;
                auth.network_error = result.network_error;
            }
            // v7: the verdict belongs to the effective profile (the route
            // the turn actually ran on); the map keeps it across switches.
            auth_probes[config.current.name] = auth;
            // The prompt marker is the user-facing feedback (2026-11-02: the
            // [auth] history row that used to go with this is deliberate noise
            // on every first reply), so a chat-driven verdict change is only
            // recorded in the diagnostics log.
            if (auth_log_token(auth) != login_before) {
                std::vector<std::pair<std::string, std::string>> login_fields = {
                    {"status", std::string(auth_log_token(auth))},
                    {"models", std::to_string(auth.model_count)},
                    {"source", "chat"},
                    {"provider",
                     config.current.name.empty() ? "(none)" : config.current.name},
                };
                // Status code only when one actually arrived (same rule as the
                // model-list event: `http_status=0` reads like a bogus reply).
                if (auth.http_status != 0)
                    login_fields.emplace_back("http_status", std::to_string(auth.http_status));
                logger->event("login", login_fields);
            }
            turn_done = false;
            // Drain the steering queue once the turn commits (Pi-style,
            // 5.6 v1.x #1). Skipped while exiting.
            if (!exiting && !pending.empty()) {
                std::string next = std::move(pending.front());
                pending.erase(pending.begin());
                if (next == "/exit") {
                    goto done;
                }
                if (!handle_slash(next)) {
                    if (config.complete()) {
                        start_turn(next);
                    } else {
                        screen.commit("[error] LLM not configured, dropped queued message ·" +
                                      config_next_hint(config));
                    }
                } else {
                    continue; // slash handled, keep rendering idle frame
                }
            }
        } else {
            render_frame_impl(slash_ctx);
        }
    }

done:
    exiting = true;
    abort_requested = true;
    set_preview_mouse(false); // 018: restore normal mouse/selection on exit
    if (worker.joinable())
        worker.join();
    if (models_worker.joinable())
        models_worker.join(); // bounded by its own 15 s curl timeout
    if (probe_worker.joinable())
        probe_worker.join(); // v4.3: bounded by its own 30 s curl timeout
    if (compact_worker.joinable())
        compact_worker.join(); // 015: abort flag set above; bounded by curl timeout
    if (sidebar_worker.joinable())
        sidebar_worker.join(); // v4.13: git scan capped at 5 s
    logger->event("session_close",
                  {{"mode", "tui"}, {"id", session.meta.id}, {"temp", temp_mode ? "1" : "0"}});
    Logger::shutdown_all();
    screen.clear_live();
    if (temp_mode) {
        screen.commit("temp session " + session.meta.id + " discarded (nothing was saved)");
    } else {
        screen.commit("saved session " + session.meta.id + " (resume: mini-agent run --session " +
                      session.meta.id + ")");
    }
    return EXIT_SUCCESS;
}

} // namespace mini_agent::tui
