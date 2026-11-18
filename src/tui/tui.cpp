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
    auto handle_slash = [&](std::string_view line) -> bool {
        auto arg_of = [](std::string_view text) -> std::string_view {
            const auto space = text.find(' ');
            return space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
        };
        if (line == "/help") {
            // Short lines on purpose: single-row commands stay intact even
            // on narrow terminals (wrap breaks mid-token at width limit).
            screen.commit("commands: /provider <url> · /login [key] · /model <name>");
            screen.commit("          /provider list|add <name> [url]|rm <name> (profiles)");
            screen.commit("          /login <name> [key] · /model <provider>/<name>");
            screen.commit("          /tools (tool registry) · /mcp (servers) · /cwd [path]");
            screen.commit("          /approve ask|allowlist|off (tool gate)");
            screen.commit("          /compact (summarize history; context relief)");
            screen.commit("          /models (list & pick) · /log <path>|off|default");
            screen.commit(
                "          /log info|debug|trace (capture level; /debug on|all|off alias)");
            screen.commit(
                "          /config · /sessions · /session <id|prefix>|new · /sysinfo on|off");
            screen.commit("          /version (build identity: version · commit · built)");
            screen.commit("          /sidebar on|off (tree + changed files)");
            screen.commit("          /preview <path> (show a file; .md renders)");
            screen.commit("          /exit");
            screen.commit("Each of /provider /login /model saves on its own;");
            screen.commit("configure them in any order, any number of sittings.");
            screen.commit("prompt: ?=unconfigured/unverified · ~=checking · >=ok");
            screen.commit("        !=key rejected · x=provider unreachable");
            screen.commit("Enter sends · Enter queues while streaming");
            return true;
        }
        if (line.rfind("/login", 0) == 0) {
            // v6 (design 012 §5.2): a first token naming a configured
            // profile targets that profile's key; anything else is the key
            // itself for the active profile. v7: with no profiles the
            // command auto-creates default (design 013 §5.4).
            const auto arg = arg_of(line);
            ensure_default_profile();
            std::string target = config.active_provider;
            std::string_view key = arg;
            if (!arg.empty()) {
                const auto space = arg.find(' ');
                const auto first = space == std::string_view::npos ? arg : arg.substr(0, space);
                if (config.find_provider(first) != nullptr) {
                    target = std::string(first);
                    key = space == std::string_view::npos ? std::string_view() : arg.substr(space);
                    while (!key.empty() && key.front() == ' ')
                        key.remove_prefix(1);
                }
            }
            const std::string label = " (" + target + ")";
            if (key.empty()) {
                secret_input = true; // masked composer; next Enter sets key
                // The profile name pins the target even if the active
                // profile changes before Enter lands.
                secret_field = "api_key:" + target;
                screen.commit("[input] API key" + label +
                              " (hidden); Enter confirms, empty Enter cancels");
            } else {
                auto* profile = config.find_provider(target);
                profile->api_key = std::string(key);
                auth_probes[target] = AuthProbe{}; // verdict belongs to the old key
                if (target == config.current.name)
                    auth = AuthProbe{};
                config.refresh_current();
                save_and_report(true);
                maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/models", 0) == 0) {
            // Listed before /model so the shared prefix resolves here first.
            // v6: any complete profile (or the unnamed route) can be listed;
            // the fetch aggregates every ready target.
            bool any_ready = !config.current.base_url.empty() && !config.current.api_key.empty();
            for (const auto& p : config.providers)
                any_ready = any_ready || (!p.base_url.empty() && !p.api_key.empty());
            if (!any_ready) {
                screen.commit("[error] model listing needs base_url + api_key ·" +
                              config_next_hint(config));
            } else if (models_running) {
                screen.commit("[info] already listing models, wait for the list");
            } else {
                screen.commit("[info] listing models from provider...");
                start_models_fetch(true); // force: explicit re-list bypasses cache
            }
            return true;
        }
        if (line.rfind("/model", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const std::string shown =
                    config.current.name.empty()
                        ? (config.current.model.empty() ? "(not set)" : config.current.model)
                        : config.current.name + "/" + config.current.model;
                screen.commit("model=" + shown + "; usage /model <name> | <provider>/<name>" +
                              (config.current.base_url.empty() || config.current.api_key.empty()
                                   ? std::string()
                                   : " · /models lists what the provider offers"));
            } else {
                // v6 (design 012 §5.2): qualified -> atomic profile switch;
                // bare -> active-profile match, then a globally unique id
                // in the last fetched list; ambiguous names fail closed
                // (never guess an endpoint).
                const std::string ref(arg);
                std::string provider;
                std::string id;
                if (split_qualified_model(ref, &provider, &id) &&
                    config.find_provider(provider) != nullptr) {
                    select_model(provider, id, "slash");
                    return true;
                }
                std::vector<std::string> owners;
                for (std::size_t i = 0; i < model_choice_targets.size(); ++i) {
                    const auto& target = model_choice_targets[i];
                    if (target.second == ref &&
                        std::find(owners.begin(), owners.end(), target.first) == owners.end())
                        owners.push_back(target.first);
                }
                if (owners.size() > 1) {
                    std::string names;
                    for (const auto& owner : owners)
                        names += (names.empty() ? "" : ", ") +
                                 (owner.empty() ? "(none)" : owner);
                    screen.commit("[error] model '" + ref +
                                  "' exists on several providers: " + names);
                    screen.commit("        pick one with /model <provider>/" + ref);
                } else if (owners.size() == 1) {
                    select_model(owners.front(), ref, "slash");
                } else {
                    // Not in the fetched list: literal set on the active
                    // route (legacy behavior -- the id may simply not be
                    // listed).
                    select_model("", ref, "slash");
                }
            }
            return true;
        }
        if (line.rfind("/provider", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty() || arg == "list") {
                // v6 (design 012 §5.2): all profiles, keys masked, active
                // marked. v7: no unnamed row -- /config shows the effective
                // values when env overrides them.
                if (config.providers.empty()) {
                    screen.commit("providers: none · usage /provider add <name> [url]");
                } else {
                    screen.commit("providers (" + std::to_string(config.providers.size()) + "):");
                    for (const auto& p : config.providers)
                        screen.commit("  " + p.name + " · " +
                                      (p.base_url.empty() ? "(url not set)" : p.base_url) +
                                      " · key=" + (p.api_key.empty() ? "(not set)" : "***set***") +
                                      (p.name == config.active_provider ? "  <- active" : ""));
                }
                screen.commit("usage /provider <url> · /provider add <name> [url] · "
                              "/provider rm <name>");
            } else if (arg.rfind("add", 0) == 0 && (arg.size() == 3 || arg[3] == ' ')) {
                auto rest = arg.substr(3);
                while (!rest.empty() && rest.front() == ' ')
                    rest.remove_prefix(1);
                const auto space = rest.find(' ');
                const std::string name(rest.substr(0, space));
                auto url = space == std::string_view::npos ? std::string_view()
                                                           : rest.substr(space + 1);
                while (!url.empty() && url.front() == ' ')
                    url.remove_prefix(1);
                ProviderProfile probe;
                probe.name = name;
                probe.base_url = std::string(url);
                std::string err;
                if (!probe.valid(&err)) {
                    screen.commit("[error] " + err);
                    return true;
                }
                if (auto* existing = config.find_provider(name)) {
                    // Upsert (same rule as MCP): URL replaces when given;
                    // key/model/metadata survive an add without a URL.
                    if (!url.empty()) {
                        existing->base_url = std::string(url);
                        auth_probes.erase(name);
                        if (name == config.current.name) {
                            auth = AuthProbe{};
                            config.refresh_current();
                        }
                    }
                    screen.commit("[ok] provider '" + name + "' updated" +
                                  (url.empty() ? "" : " · " + std::string(url)));
                } else {
                    config.providers.push_back(std::move(probe));
                    // v7 (design 013 §5.4): the first profile auto-activates
                    // so the table never exists without a selection.
                    if (config.providers.size() == 1) {
                        config.active_provider = name;
                        config.refresh_current();
                        switch_active_profile();
                    }
                    screen.commit("[ok] provider '" + name + "' added" +
                                  (url.empty() ? "" : " · " + std::string(url)));
                }
                const auto* added = config.find_provider(name);
                if (added->base_url.empty())
                    screen.commit("     next: /provider add " + name +
                                  " <url> · then /login " + name);
                else if (added->api_key.empty())
                    screen.commit("     next: /login " + name + " (key stays hidden)");
                logger->event("provider_add", {{"provider", name}});
                save_and_report(true);
                maybe_autofetch_models();
            } else if (arg.rfind("rm", 0) == 0 && (arg.size() == 2 || arg[2] == ' ')) {
                auto rest = arg.substr(2);
                while (!rest.empty() && rest.front() == ' ')
                    rest.remove_prefix(1);
                const std::string name(rest);
                auto it = std::find_if(config.providers.begin(), config.providers.end(),
                                       [&](const ProviderProfile& p) { return p.name == name; });
                if (name.empty() || it == config.providers.end()) {
                    screen.commit("[error] unknown provider '" + name +
                                  "' · /provider lists the names");
                    return true;
                }
                const bool was_active = name == config.active_provider;
                config.providers.erase(it);
                auth_probes.erase(name);
                if (was_active) {
                    // v7 (design 013 §5.4, Q2): fall back to the first
                    // remaining profile; with none left the setup state
                    // returns. The removed profile's credentials go with it.
                    if (!config.providers.empty())
                        config.activate_provider(config.providers.front().name);
                    else
                        config.active_provider.clear();
                    config.refresh_current();
                    switch_active_profile();
                    screen.commit(config.providers.empty()
                                      ? "[info] active profile removed; no profiles left · "
                                        "/provider add <name> or /login to start over"
                                      : "[info] active profile removed; switched to '" +
                                            config.active_provider + "'");
                }
                logger->event("provider_rm", {{"provider", name}});
                save_and_report(true);
                screen.commit("[ok] provider '" + name + "' removed");
            } else {
                // /provider <url>: edit the active profile's URL (v7: no
                // unnamed top level; auto-default when no profile exists).
                ensure_default_profile();
                ProviderProfile probe;
                probe.name = config.active_provider;
                probe.base_url = std::string(arg);
                std::string err;
                if (!probe.valid(&err)) {
                    screen.commit("[error] " + err);
                    return true;
                }
                auto* active = config.find_provider(config.active_provider);
                active->base_url = probe.base_url;
                while (!active->base_url.empty() && active->base_url.back() == '/')
                    active->base_url.pop_back();
                auth_probes.erase(active->name);
                if (active->name == config.current.name)
                    auth = AuthProbe{}; // a different endpoint cannot inherit it
                config.refresh_current();
                save_and_report(true);
                maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/log", 0) == 0) {
            // v4.8 (design 010 §19): /log owns both the metadata-log path
            // (pre-v4.8 behavior, unchanged) and the traffic-capture
            // level ladder: info (default, metadata only) < debug (raw
            // chat dumps) < trace (+ truncated auxiliary traffic).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(logger->enabled() ? "logging on: " + config.log_file
                                                : "logging off; usage /log <path> or /log off");
                screen.commit("level=" + config.log_level +
                              " (session-only) · usage /log info|debug|trace");
            } else if (arg == "info" || arg == "debug" || arg == "trace") {
                // v4.8.1: the level is session-scoped -- set in memory,
                // never saved (persistence would resurrect a debug switch
                // across sessions; the maintainer rule for this knob).
                config.log_level = std::string(arg);
                if (config.log_level == "info") {
                    screen.commit("[ok] log level info (this session) · metadata log only, "
                                  "no raw dumps");
                } else {
                    screen.commit("[ok] log level " + config.log_level + " (this session)" +
                                  (config.log_level == "debug"
                                       ? " · raw chat traffic"
                                       : " · chat + truncated aux traffic") +
                                  " -> " + config.to_provider().debug_log_path);
                }
            } else if (arg == "off") {
                config.log_file = "off"; // sentinel: explicit disable
                logger = make_logger();
                save_and_report();
                screen.commit("[ok] logging disabled");
            } else if (arg == "default") {
                config.log_file.clear(); // empty -> /tmp/mini-agent-<uid>.log
                logger = make_logger();
                save_and_report();
                const auto shown = Logger::resolve_path(config);
                screen.commit("[ok] logging back to default: " +
                              (shown ? *shown : std::string("(none)")));
            } else {
                config.log_file = std::string(arg);
                logger = make_logger();
                save_and_report();
                if (logger->enabled()) {
                    screen.commit("[ok] logging to: " + config.log_file);
                } else {
                    screen.commit("[warn] log path not writable, logging stays off");
                }
            }
            return true;
        }
        if (line.rfind("/compact", 0) == 0) {
            // 015 (design 015 §4C): manual history compaction. Idle-gated
            // like /session; the summary request runs on its own worker.
            if (streaming || compact_running || approval_pending()) {
                screen.commit("[info] /compact needs an idle turn · wait or Ctrl-C");
                return true;
            }
            std::size_t count = 0;
            for (const auto& m : session.chat.messages()) {
                if (m.role != Role::System)
                    ++count;
            }
            if (count < 2) {
                screen.commit("[info] nothing to compact (fewer than 2 messages)");
                return true;
            }
            // 017: if the whole history fits the retained-tail budget there
            // is nothing to summarize -- compacting would only add an empty
            // checkpoint (same guard family as "cannot reclaim, do not run").
            const std::size_t keep_from = find_retained_tail(session.chat.messages(), 20000);
            std::size_t region_non_system = 0;
            for (std::size_t i = 0; i < keep_from; ++i) {
                if (session.chat.messages()[i].role != Role::System)
                    ++region_non_system;
            }
            if (region_non_system == 0) {
                screen.commit("[info] nothing to compact (history fits the retained tail budget)");
                return true;
            }
            if (!config.complete()) {
                screen.commit("[error] LLM not configured ·" + config_next_hint(config));
                return true;
            }
            // 017 (design 017 §2.2): verbatim backup BEFORE any replacement;
            // a failed backup aborts the compact (history stays untouched).
            if (!temp_mode) {
                std::string backup_err;
                if (!store.save_backup(session, &backup_err)) {
                    screen.commit("[error] compact backup failed: " + backup_err +
                                  " (history unchanged)");
                    return true;
                }
            }
            const AppConfig snapshot = config; // thread boundary
            const Chat history = session.chat; // thread boundary
            compact_result = CompactResult{};
            compact_done = false;
            compact_running = true;
            abort_requested = false;
            logger->event("compact", {{"status", "start"},
                                      {"before", std::to_string(history.messages().size())}});
            screen.commit("[info] compacting " + std::to_string(history.messages().size()) +
                          " messages (recent turns kept verbatim, original backed up; "
                          "Ctrl-C/Esc cancels)...");
            compact_worker = std::thread([&, snapshot, history] {
                compact_result =
                    compact_history(snapshot, history, [&] { return abort_requested.load(); });
                compact_done = true;
            });
            return true;
        }
        if (line == "/config") {
            screen.commit("base_url=" + (config.current.base_url.empty() ? "(not set)" : config.current.base_url));
            screen.commit("model=" + (config.current.model.empty() ? "(not set)" : config.current.model));
            screen.commit("api_key=" +
                          std::string(config.current.api_key.empty() ? "(not set)" : "***set***"));
            screen.commit("login=" + std::string(auth_log_token(auth)));
            // v6 (design 012): profile table size + active selection.
            screen.commit("providers=" + std::to_string(config.providers.size()) + " active=" +
                          (config.active_provider.empty() ? "(no provider)" : config.active_provider) +
                          (config.env_override_active ? " · env override in effect" : ""));
            screen.commit("log_file=" + (config.log_file.empty() ? "(off)" : config.log_file));
            screen.commit(std::string("log_level=") + config.log_level +
                          " (session-only, not saved)");
            // v4 fields (design 010).
            screen.commit("context_window=" +
                          (config.current.context_window > 0
                               ? format_token_count(config.current.context_window) + " (" +
                                     std::to_string(config.current.context_window) + ")"
                               : std::string("(not set)")));
            screen.commit("reasoning_effort=" + (config.current.reasoning_effort.empty()
                                                     ? std::string("(not set)")
                                                     : config.current.reasoning_effort));
            screen.commit("config file: " + default_config_path().string() +
                          " (env overrides file)");
            return true;
        }
        if (line.rfind("/sidebar", 0) == 0) {
            // v4.13: right sidebar with the workspace tree + changed
            // files. Session-only display state (same rule as /sysinfo).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(std::string("sidebar=") + (show_sidebar ? "on" : "off") +
                              " · usage /sidebar on|off");
            } else if (arg == "on") {
                show_sidebar = true;
                start_sidebar();
                screen.commit("[ok] sidebar on (this session) · tree + git changes");
            } else if (arg == "off") {
                show_sidebar = false;
                sidebar_focus = false; // v4.14: no focus survives the panel
                screen.commit("[ok] sidebar off (this session)");
            } else {
                screen.commit("[error] /sidebar expects on|off");
            }
            return true;
        }
        if (line.rfind("/sysinfo", 0) == 0) {
            // v4.12: bottom system-info row toggle (user@host · cwd ·
            // clock). Session-only like the log level -- display state,
            // not a durable preference.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit(std::string("sysinfo=") + (show_sysinfo ? "on" : "off") +
                              " · usage /sysinfo on|off");
            } else if (arg == "on") {
                show_sysinfo = true;
                screen.commit("[ok] system info row on (session-only)");
            } else if (arg == "off") {
                show_sysinfo = false;
                screen.commit("[ok] system info row off (session-only)");
            } else {
                screen.commit("[error] /sysinfo expects on|off");
            }
            return true;
        }
        if (line.rfind("/version", 0) == 0) {
            // 0.8.0: runtime build identity, same three lines as the CLI
            // `version` subcommand. Read-only; works in any state.
            screen.commit("mini-agent v" MINI_AGENT_VERSION);
            screen.commit("commit " MINI_AGENT_COMMIT);
            screen.commit("built " MINI_AGENT_BUILD_TIME);
            return true;
        }
        if (line == "/sessions") {
            std::string err;
            const auto ids = store.list(&err);
            screen.commit(ids.empty() ? "sessions: (empty)" : "sessions:");
            for (const auto& id : ids)
                screen.commit("  " + id + (id == session.meta.id ? "  <- current" : ""));
            screen.commit("switch in place: /session <id or prefix> · /session new");
            return true;
        }
        if (line.rfind("/session", 0) == 0 && line != "/sessions") {
            // v4.9: switch or fork the live session without exiting. Same
            // idle-gated swap discipline as /cwd (worker join, registry
            // rebuild); the old session is saved and unlocked, the target
            // is loaded and locked. A failed lock/load rolls back.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("session=" + session.meta.id +
                              " · usage /session <id or prefix> | /session new");
                return true;
            }
            if (streaming || turn_done.load() || compact_running) {
                // The turn-completion handler writes session.chat after
                // joining; swapping mid-fold would land the old turn in
                // the new session. Require idle (Ctrl-C interrupts).
                screen.commit("[info] session switch needs an idle turn · wait or Ctrl-C");
                return true;
            }
            std::string target_id = std::string(arg);
            const bool fresh = target_id == "new";
            if (fresh) {
                target_id = mini_agent::generate_session_id();
            } else if (!mini_agent::session_id_is_safe(target_id)) {
                screen.commit("[error] unsafe session id");
                return true;
            } else {
                // v4.13: prefix match (maintainer request). The typed word
                // resolves against stored ids; one hit replaces it, several
                // list candidates, none falls through so `load` reports the
                // usual "no such session" error.
                session_ids_stale = true;
                refresh_session_ids();
                const auto m = mini_agent::match_session_ref(target_id, session_ids);
                if (m.exact) {
                    target_id = m.id;
                } else if (!m.candidates.empty()) {
                    screen.commit("session " + target_id + " is ambiguous:");
                    for (const auto& id : m.candidates)
                        screen.commit("  " + id + (id == session.meta.id ? "  <- current" : ""));
                    screen.commit("type more characters, or Tab to cycle /session <id>");
                    return true;
                }
            }
            if (target_id == session.meta.id) {
                screen.commit("[info] already on session " + target_id);
                return true;
            }
            std::string err;
            // Persist the outgoing session first; a save failure is a warn
            // only (the switch proceeds; the old file keeps last-turn state).
            // v4.15: a temp session has no file and never gets one, so the
            // pre-switch save is skipped (its chat is dropped on switch-out).
            if (!temp_mode && !store.save(session, &err)) {
                screen.commit("[warn] save before switch failed: " + err);
            }
            Session next;
            std::optional<std::string> next_workspace;
            if (fresh) {
                next.meta.id = target_id;
                next.meta.model = config.current.model;
                next_workspace = config.workspace.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>(config.workspace);
                next.meta.workspace = next_workspace;
                next.meta.provider =
                    config.current.name.empty()
                        ? std::optional<std::string>()
                        : std::optional<std::string>(config.current.name);
                mini_agent::inject_system_prompt(next.chat, config.workspace, config.current.model,
                                                 config.system_prompt);
            } else {
                auto loaded = store.load(target_id, &err);
                if (!loaded) {
                    screen.commit("[error] load failed: " + sanitize_display(err));
                    return true;
                }
                next = std::move(*loaded);
            }
            if (!store.try_lock(target_id, &err)) {
                screen.commit("[error] session busy: " + sanitize_display(err));
                return true;
            }
            store.unlock(session.meta.id);
            session = std::move(next);
            active_session_id = session.meta.id;
            // v4.15: `/session new` from a temp session stays temp (the new
            // chat is equally ephemeral); resuming a saved id ends temp mode
            // -- from here on the target behaves like a normal session.
            temp_mode = temp_mode && fresh;
            // v3 workspace precedence on switch (same as resume in cmd_run):
            // the session's own stamp rebinds config; else keep current.
            if (session.meta.workspace && !session.meta.workspace->empty()) {
                config.workspace = *session.meta.workspace;
                session.chat.update_system_workspace(config.workspace);
                std::string note;
                tool_registry = build_tool_registry(config, &note);
            }
            config_snapshot = config; // worker turn snapshot refresh (same as /cwd)
            reset_usage_readings();   // usage belongs to the new session
            // Re-seed ↑/↓ history from the new chat (same rule as launch).
            history.clear();
            for (const auto& m : session.chat.messages())
                if (m.role == Role::User && !m.content.empty() && m.content.front() != '/')
                    history.push_back(m.content);
            hist_pos = history.size();
            history_draft.clear();
            logger->event("session_switch", {{"id", session.meta.id},
                                             {"fresh", fresh ? "new" : "resume"},
                                             {"temp", temp_mode ? "on" : "off"}});
            screen.commit(std::string("[ok] ") + (fresh ? "new" : "resumed") +
                          (temp_mode ? " temp" : "") + " session " + session.meta.id + " · " +
                          std::to_string(session.chat.messages().size()) + " message(s)");
            return true;
        }
        if (line.rfind("/context", 0) == 0) {
            // v4 (design 010): model context window for the status-bar
            // ctx% reading. Config is authoritative -- no probing.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const auto inputs = context_reading();
                std::string shown = config.current.context_window > 0
                                        ? format_token_count(config.current.context_window) + " (" +
                                              std::to_string(config.current.context_window) + ")"
                                        : std::string("(not set)");
                // v4.7: when unset, surface the catalog reading (the same
                // value the status bar silently uses) so the user can see
                // and confirm it before it becomes config-authoritative.
                if (config.current.context_window <= 0 && inputs.context_window > 0)
                    shown += " · catalog: " + format_token_count(inputs.context_window) + " (" +
                             std::to_string(inputs.context_window) + ") · set it: /context " +
                             std::to_string(inputs.context_window);
                screen.commit("context_window=" + shown +
                              "; usage /context <tokens|128K|1M> | /context probe | /context off");
                screen.commit(
                    "ctx reading: " + std::string(inputs.context_exceeded ? "EXCEEDED " : "") +
                    (inputs.context_tokens > 0
                         ? (inputs.context_estimated ? "~" : "") +
                               format_token_count(inputs.context_tokens) + " tokens (" +
                               std::to_string(inputs.context_tokens) + ")" +
                               (inputs.context_window > 0
                                    ? " = " +
                                          std::to_string(inputs.context_tokens * 100 /
                                                         inputs.context_window) +
                                          "% of window"
                                    : std::string())
                         : std::string("(no data yet)")) +
                    (inputs.context_estimated ? " · estimated" : " · provider-anchored"));
                // 017 (design 017 §2.1): per-class breakdown + pressure.
                screen.commit("breakdown (estimated): system ~" +
                              format_token_count(inputs.breakdown.system) + " · conversation ~" +
                              format_token_count(inputs.breakdown.conversation) +
                              " · tool output ~" +
                              format_token_count(inputs.breakdown.tool_output));
                if (inputs.context_exceeded)
                    screen.commit("[warn] last request overflowed the window · reading "
                                  "synthesized until the next successful turn");
                if (const long long threshold = pressure_threshold(inputs.context_window);
                    threshold > 0)
                    screen.commit("pressure: /compact hint at >= " + format_token_count(threshold) +
                                  " tokens (window - max(15%, 16k))");
            } else if (arg == "off" || arg == "0") {
                active_profile_for_edit()->context_window = 0;
                config.refresh_current(); // v7: per-profile metadata
                save_and_report();
                screen.commit("[ok] context window cleared · status shows tokens, no %");
            } else if (arg == "probe") {
                // v4.3 (design 010 §13): explicit, user-triggered cheap
                // probe -- one oversized-max_tokens request whose rejection
                // carries the gateway's authoritative numbers. Never
                // automatic: it costs a request and the message text is
                // not an API contract, so the maintainer decides when.
                if (probe_running) {
                    screen.commit("[info] probe already in flight, wait for the result");
                    return true;
                }
                if (config.current.base_url.empty() || config.current.api_key.empty() || config.current.model.empty()) {
                    screen.commit("[error] probe needs base_url + api_key + model ·" +
                                  config_next_hint(config));
                    return true;
                }
                if (probe_worker.joinable())
                    probe_worker.join();
                probe_running = true;
                probe_done = false;
                probe_result = ProbeResult{};
                logger->event("ctx_probe", {{"status", "start"},
                                            {"base_url", Logger::redact_url(config.current.base_url)},
                                            {"model", config.current.model}});
                const AppConfig snapshot = config; // thread boundary
                probe_worker = std::thread([&, snapshot] {
                    probe_result = probe_window(snapshot);
                    probe_done = true;
                });
                screen.commit("[info] probing context window (one oversized request)...");
            } else {
                // §21: decimal K/M suffixes (128K=128000, 1M=1000000);
                // plain digits stay exact. 0 = invalid, nothing written.
                const long long parsed_window = parse_token_count(arg);
                if (parsed_window <= 0) {
                    screen.commit("[error] /context expects a positive token count "
                                  "(e.g. 131072, 128K, 1M)");
                    return true;
                }
                active_profile_for_edit()->context_window = parsed_window;
                config.refresh_current(); // v7: per-profile metadata
                save_and_report();
            }
            return true;
        }
        if (line.rfind("/reasoning", 0) == 0) {
            // v4 (design 010): reasoning_effort passthrough. Verbatim
            // value; the server is the arbiter (400 -> hint, no auto-drop:
            // config is authoritative and silent removal would lie about
            // what was sent).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("reasoning_effort=" +
                              (config.current.reasoning_effort.empty() ? std::string("(not set)")
                                                               : config.current.reasoning_effort) +
                              "; usage /reasoning <level> or /reasoning off");
                // v4.2 (§11): cached capabilities for the current model ->
                // numbered picker over the gateway's supported levels (the
                // same interaction as /models). A literal value still works
                // for models without advertised metadata.
                const auto caps = caps_for_model();
                if (!caps.reasoning_efforts.empty()) {
                    close_preview_pane(); // 018: menu takes over the live region
                    reasoning_choices = caps.reasoning_efforts;
                    reasoning_menu_open = true;
                    for (const auto& row : reasoning_menu_rows(
                             reasoning_choices, config.current.reasoning_effort,
                             caps.reasoning_default_effort, caps.reasoning_mandatory))
                        screen.commit(row);
                } else if (caps.supports_reasoning) {
                    screen.commit("[info] gateway marks this model as reasoning-capable "
                                  "but lists no levels; set one explicitly");
                }
            } else if (arg == "off") {
                active_profile_for_edit()->reasoning_effort.clear();
                config.refresh_current(); // v7: per-profile metadata
                save_and_report();
                screen.commit("[ok] reasoning effort cleared · requests omit the field");
            } else {
                active_profile_for_edit()->reasoning_effort = std::string(arg);
                config.refresh_current(); // v7: per-profile metadata
                save_and_report();
            }
            return true;
        }
        if (line.rfind("/debug", 0) == 0) {
            // v4.8 (design 010 §19): /debug is now an alias of the /log
            // level ladder -- on == /log debug, all == /log trace, off ==
            // /log info. Bodies contain conversation content (the point
            // of the level) while the api_key stays in the Authorization
            // header only.
            const auto arg = arg_of(line);
            auto set_level = [&](std::string_view level, std::string_view ok_text) {
                config.log_level = level; // session-scoped (v4.8.1): no save
                if (config.log_level == "info") {
                    screen.commit(std::string(ok_text) + " · metadata log only");
                } else {
                    screen.commit(std::string(ok_text) + " -> " +
                                  config.to_provider().debug_log_path);
                }
            };
            if (arg.empty()) {
                const auto pc = config.to_provider();
                screen.commit(
                    std::string("debug=") + config.log_level +
                    (config.capture_chat() ? " · file: " + pc.debug_log_path : std::string()) +
                    "; usage /debug on|all|off (alias of /log debug|trace|info)");
            } else if (arg == "on") {
                set_level("debug", "[ok] log level debug (chat traffic) · raw chat traffic");
            } else if (arg == "all") {
                // trace: ALSO dump auxiliary traffic (models listing,
                // context probe request/response), truncated.
                set_level("trace", "[ok] log level trace (chat + truncated aux traffic)");
            } else if (arg == "off") {
                set_level("info", "[ok] log level info · no further dumps");
            } else if (arg == "debug" || arg == "trace" || arg == "info") {
                set_level(arg, arg == "info"
                                   ? "[ok] log level info"
                                   : (arg == "debug" ? "[ok] log level debug (chat traffic)"
                                                     : "[ok] log level trace (chat + aux)"));
            } else {
                screen.commit("[error] /debug expects on|all|off|info|debug|trace");
            }
            return true;
        }
        if (line.rfind("/approve", 0) == 0) {
            // v3.1 runtime approval switch: /approve [ask|allowlist|off].
            // Worker must be idle (policy is snapshotted per turn), same
            // join discipline as /cwd and /tools.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("approval=" + approval_policy +
                              " · /approve ask|allowlist|off · 'a' key also "
                              "allows a tool name for this session");
                if (approval_policy == "allowlist") {
                    std::string joined;
                    for (const auto& name : config.tool_allowlist) {
                        if (!joined.empty())
                            joined += ", ";
                        joined += name;
                    }
                    screen.commit("allowlist: " +
                                  (joined.empty() ? std::string("(empty)") : joined));
                }
                return true;
            }
            const std::string want(arg);
            if (want != "ask" && want != "allowlist" && want != "off") {
                screen.commit("[error] /approve expects ask|allowlist|off");
                return true;
            }
            if (worker.joinable())
                worker.join();
            approval_policy = want;
            config.tool_approval = want;
            config_snapshot = config; // worker turn snapshot refresh (same as /cwd)
            save_and_report(false);
            if (want == "off" && !tool_registry->empty())
                screen.commit("[warn] tool_approval=off: tools run WITHOUT asking"
                              " (no sandbox: run_bash executes at your user's "
                              "privileges; git push stays human-only by tool "
                              "absence, but shell can still reach remotes)");
            else if (want == "allowlist" && config.tool_allowlist.empty())
                screen.commit("[info] allowlist mode with an EMPTY list: every tool "
                              "still asks; add names via tool_allowlist in config.json");
            else
                screen.commit("[ok] approval=" + approval_policy + " saved");
            logger->event("approval_switch", {{"policy", approval_policy}});
            return true;
        }
        if (line.rfind("/cwd", 0) == 0) {
            // v3 (design 009 5.4): query or rebind the session workspace.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("workspace: " +
                              sanitize_display(config.workspace.empty()
                                                   ? std::filesystem::current_path().string()
                                                   : config.workspace) +
                              " · /cwd <path> to rebind (file+git tools and run_bash)");
                return true;
            }
            const std::filesystem::path target(arg);
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(target, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                screen.commit("[error] not a directory: " + sanitize_display(arg));
                return true;
            }
            if (worker.joinable())
                worker.join(); // registry swap needs the worker idle (008 thread rule)
            config.workspace = canonical.string();
            session.meta.workspace = config.workspace;
            session.chat.update_system_workspace(config.workspace);
            std::string note;
            tool_registry = build_tool_registry(config, &note);
            config_snapshot = config;
            sidebar_browse.clear(); // 018: browser resets to the new workspace
            start_sidebar(); // v4.13: tree belongs to the new boundary
            screen.commit("[ok] workspace=" + sanitize_display(config.workspace) +
                          " · tools rebound · " +
                          (config.workspace == std::filesystem::current_path().string()
                               ? "(matches launch cwd)"
                               : "run_bash now starts inside it"));
            return true;
        }
        if (line.rfind("/preview", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                screen.commit("usage: /preview <path> (workspace-bounded; .md renders)");
                return true;
            }
            open_preview_pane(std::string(arg));
            return true;
        }
        if (line == "/tools") {
            // Rebuild the registry so fresh server configs apply without
            // a restart; old clients die with their transports (RAII).
            if (worker.joinable())
                worker.join();
            config_snapshot = config;
            std::string note;
            tool_registry = build_tool_registry(config, &note);
            std::vector<std::string> rendered;
            for (const auto& definition : tool_registry->list()) {
                auto first_line = definition.description.find('\n');
                rendered.push_back(definition.name + " \u00b7 " + definition.source + " \u00b7 " +
                                   sanitize_display(definition.description.substr(0, first_line)));
            }
            for (const auto& row : tool_registry_rows(rendered))
                screen.commit(row);
            if (!note.empty())
                screen.commit("[info] " + note + " (lazy start on first call)");
            screen.commit("approval=" + approval_policy);
            return true;
        }
        if (line == "/mcp") {
            if (config.mcp_servers.empty()) {
                screen.commit("mcp servers: (none) · add mcp_servers to config.json");
                return true;
            }
            for (const auto& server : config.mcp_servers) {
                screen.commit("  " + server.name + " \u00b7 " + server.transport + "\u2192" +
                              (server.transport == "http" ? Logger::redact_url(server.url)
                                                          : sanitize_display(server.command)));
            }
            screen.commit("restart servers after config edits: /tools · down marks clear per call");
            return true;
        }
        if (line == "/exit")
            return false; // caller exits
        if (!line.empty() && line.front() == '/') {
            screen.commit("[error] unknown command " + std::string(line) + " (/help)");
            return true;
        }
        return false;
    };

    auto hist_up = [&] {
        if (hist_pos == 0)
            return;
        if (hist_pos == history.size())
            history_draft = composer; // leaving the draft slot: keep it
        composer = history[--hist_pos];
        cursor = composer.size();
    };
    auto hist_down = [&] {
        if (hist_pos == history.size())
            return;
        if (++hist_pos == history.size()) {
            composer = history_draft;
            history_draft.clear();
        } else {
            composer = history[hist_pos];
        }
        cursor = composer.size();
    };
    // Editing a recalled line parks the edit in the draft slot without
    // moving hist_pos (bash semantics): Down returns to it later.
    auto mark_edited = [&] {
        if (hist_pos != history.size())
            history_draft = composer;
    };

    // 018: insert the highlighted entry (file OR directory) as a workspace-
    // relative path and leave focus. Enter uses this for files; `i` uses it for
    // any entry, so a directory can still be handed to the model now that Enter
    // navigates into it. Defined here (after mark_edited) for scope.
    auto insert_sidebar_selection = [&]() -> bool {
        if (sidebar_sel >= sidebar_result.paths.size() || sidebar_result.paths[sidebar_sel].empty())
            return false;
        const std::string& rawp = sidebar_result.paths[sidebar_sel];
        std::string rel = rawp;
        if (!rel.empty() && rel.front() == '/') {
            std::error_code rec;
            const auto ws = config.workspace.empty() ? std::filesystem::current_path()
                                                     : std::filesystem::path(config.workspace);
            const auto r = std::filesystem::relative(std::filesystem::path(rel), ws, rec);
            if (!rec)
                rel = r.string();
        }
        utf8_insert(composer, cursor, rel);
        mark_edited();
        screen.commit("[sidebar] inserted: " + sanitize_display(rel) +
                      " · edit freely, Enter sends as usual");
        sidebar_focus = false;
        return true;
    };

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
            const auto cols = static_cast<std::size_t>(term_columns());
            drain_tool_events();                 // [tool] rows land in history as they arrive
            std::vector<std::string> extra_rows; // approval block when pending
            const std::string tail = streaming ? bridge.deltas_snapshot() : std::string{};
            // v4.11 turn panel: while streaming, the extra-rows slot shows
            // the process (thinking marker + tool rows incl. the running
            // call). Approval rows keep priority when a decision is pending
            // (the pending call IS the process at that moment).
            if (streaming && !approval_pending()) {
                std::vector<std::string> panel_tools = panel_tool_rows;
                if (const auto running = bridge.running_tool(); !running.empty())
                    panel_tools.push_back(running);
                const bool thinking = reasoning_live.load(std::memory_order_relaxed);
                const auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(steady_now() - turn_start)
                        .count();
                extra_rows = turn_panel_rows(colors_enabled(), thinking, secs, panel_tools, 5);
            }
            std::string composer_display;
            if (secret_input) {
                composer_display.assign(composer.size(), '*');
                if (composer_display.empty())
                    composer_display = "(type key, Enter to confirm)";
            } else if (composer.empty()) {
                // Caret-only cell keeps the reverse-video block visible at
                // an empty prompt; the queue hint trails it.
                composer_display = "\x1b[7m \x1b[27m";
                if (streaming)
                    composer_display += "(streaming… type to queue)";
            } else {
                // Reverse-video block on the codepoint under the caret (or a
                // blank at line end). md::wrap treats the SGR as zero width,
                // so row accounting (5.6 rule 2) is unaffected.
                composer_display = composer.substr(0, cursor);
                composer_display += "\x1b[7m";
                if (cursor < composer.size()) {
                    const std::size_t nxt = utf8_next(composer, cursor);
                    composer_display.append(composer, cursor, nxt - cursor);
                    composer_display += "\x1b[27m";
                    composer_display.append(composer, nxt, std::string::npos);
                } else {
                    composer_display += ' ';
                }
                composer_display += "\x1b[27m";
            }
            // Masked input passes an empty raw composer: no slash hint.
            const std::string raw_for_hint = secret_input ? std::string() : composer;
            // v4.13: while the composer sits in the /session arg tier
            // ("/session …"), keep the id snapshot current so Tab and the
            // hint list see sessions saved by this or another process.
            if (!session_ids_stale && starts_with(raw_for_hint, "/session "))
                session_ids_stale = true;
            if (session_ids_stale)
                refresh_session_ids();
            // Prompt marker = provider connection state, derived purely from
            // (config, last probe, listing-in-flight): an idle tick yields the
            // same text, so render() keeps its zero-work input cache.
            const std::string prompt =
                prompt_marker(connection_state(config, auth, models_running));
            // Status-row colouring (2026-11-04 readability request): the
            // identity prefix dims, state words carry the hue (listing/
            // streaming cyan, idle dim, auto-tools red, AWAITING APPROVAL
            // bold yellow). SGR is zero-width for md::clip, and the clip
            // keeps any active span (it re-emits escapes while cutting).
            const bool sc = colors_enabled();
            // Status segment order: state words FIRST, identity and usage
            // last. The v4.16 merged footer clips the status to whatever
            // the sysinfo leaves (v7.2 guarantees >=24 cols on wide
            // terminals); with the identity prefix first, the streaming/
            // queued/auto-tools/approval markers were exactly the part
            // that got clipped away. State is the part that must survive;
            // model/session also live in the banner.
            std::string status;
            if (models_running) {
                status += tint(sc, "36", "listing models...");
            } else if (model_menu_open) {
                status += tint(sc, "36", "pick a model (Esc cancels)");
            }
            if (compact_running) {
                status +=
                    tint(sc, "36", (status.empty() ? "" : " · ") + std::string("compacting..."));
            }
            if (streaming) {
                // Interrupt keys stay documented in /help and the composer
                // hint; elapsed seconds live in the turn panel header. The
                // status word must fit the footer clip budget (v7.2: >=24
                // cols on an 80-col terminal) WITH the queue depth attached.
                status += tint(sc, "36", (status.empty() ? "" : " · ") + std::string("streaming"));
                if (!pending.empty()) {
                    status += tint(sc, "36", " · queued " + std::to_string(pending.size()));
                }
            } else if (status.empty()) {
                status += tint(sc, "2", "idle");
            }
            if (approval_policy == "off" && !tool_registry->empty())
                status += tint(sc, "1;31",
                               (status.empty() ? "" : " · ") +
                                   std::string("auto-tools")); // persistent danger marker (008 5.4)
            if (approval_pending()) {
                status += tint(sc, "1;33",
                               (status.empty() ? "" : " · ") + std::string("AWAITING APPROVAL"));
                // v4.17: after 90 s of the 120 s auto-deny budget, show the
                // remaining seconds. The line changes once per second, which
                // naturally busts render()'s input cache (no per-frame
                // repaint otherwise).
                if (const long long waited_s = bridge.approval_waited_s();
                    waited_s >= kApprovalWarnAtS) {
                    const long long left = std::max<long long>(0, kApprovalTimeoutS - waited_s);
                    status += tint(sc, "1;31", " · auto-deny in " + std::to_string(left) + "s");
                }
            }
            // v4.15: the id follows /session switches (like the banner) and
            // carries the temp tag; was options.session_id (launch snapshot).
            status += tint(sc, "2",
                           (status.empty() ? "" : " · ") + std::string("model=") +
                               (config.current.model.empty() ? std::string("(not set)")
                                                             : config.current.model) +
                               " · session=" + active_session_id +
                               (temp_mode ? " (temp)" : std::string()));
            // v4: usage tail (ctx/out/reasoning) last: least critical under
            // the footer clip, and /context still reports it in full.
            status += status_usage_segments(sc, context_reading());
            if (approval_pending()) {
                const ToolCall snapshot = bridge.approval_call();
                const std::string snapshot_preview = bridge.approval_preview();
                extra_rows = approval_rows(snapshot.name, snapshot.arguments_json,
                                           approval_expanded, snapshot_preview);
            }
            // 018: the preview pane owns the extra-rows slot when idle
            // (streaming/approval cannot coexist with it; menus close it).
            // Scroll/resize change extra_rows, busting the render cache.
            if (preview_pane.open && !streaming && !approval_pending()) {
                if (preview_pane.rendered_cols != cols_now()) {
                    preview_pane.lines = render_preview_lines(colors_enabled(), preview_pane.body,
                                                              preview_pane.path, cols_now());
                    preview_pane.rendered_cols = cols_now();
                }
                const std::size_t vis = pane_visible_rows(static_cast<std::size_t>(term_rows()));
                preview_pane.scroll =
                    pane_scroll_clamp(preview_pane.scroll, preview_pane.lines.size(), vis);
                extra_rows.clear();
                extra_rows.push_back(pane_header(preview_pane.path, preview_pane.scroll, vis,
                                                 preview_pane.lines.size(), cols_now()));
                const std::size_t end =
                    std::min(preview_pane.lines.size(), preview_pane.scroll + vis);
                for (std::size_t k = preview_pane.scroll; k < end; ++k)
                    extra_rows.push_back(preview_pane.lines[k]);
            }
            // v4.12: bottom system-info row (user@host · cwd · clock).
            // Gathered per frame: the clock is the only fast-changing part
            // and row-level diffing keeps the repaint cost at one row per
            // second. Home abbreviated to ~ like the Pi footer; workspace
            // stamp (session boundary) shown when set, else process cwd.
            std::string sysinfo;
            if (show_sysinfo) {
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
                std::string cwd = config.workspace.empty()
                                      ? std::filesystem::current_path().string()
                                      : config.workspace;
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
            if (show_sidebar) {
                std::vector<std::string> raw = sidebar_result.rows;
                if (!sidebar_result.error.empty())
                    raw = {"sidebar: " + sidebar_result.error};
                else if (sidebar_running)
                    raw.push_back("(scanning...)");
                sidebar_rows.push_back(tint(sc, "1", "workspace"));
                for (const auto& row : raw)
                    sidebar_rows.push_back(tint(sc, "2", sanitize_display(row)));
            }
            const std::size_t sel = sidebar_focus && sidebar_sel < sidebar_result.rows.size()
                                        ? sidebar_sel + 1 // +1: header row shifts every entry
                                        : static_cast<std::size_t>(-1);
            screen.render(tail, last_user, raw_for_hint, composer_display, prompt, status,
                          streaming, cols, extra_rows, sysinfo, session_ids, sidebar_rows, sel,
                          provider_name_list(), model_choices);
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
