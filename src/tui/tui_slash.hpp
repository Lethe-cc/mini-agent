#pragma once
// TUI slash dispatcher (019 P4 / §8). Template over run_loop context.
#include <string_view>
#include <nlohmann/json.hpp>
#include "tui_workers.hpp"
#include "tui_turn.hpp"
#include "tui_sidebar.hpp"
#include "mini_agent/tui.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/highlight.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/models_dev.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/context.hpp"

namespace mini_agent::tui {
template <class Ctx>
bool handle_slash_impl(Ctx& S, std::string_view line) {
        auto arg_of = [](std::string_view text) -> std::string_view {
            const auto space = text.find(' ');
            return space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
        };
        if (line == "/help") {
            // Short lines on purpose: single-row commands stay intact even
            // on narrow terminals (wrap breaks mid-token at width limit).
            S.screen.commit("commands: /provider <url> · /login [key] · /model <name>");
            S.screen.commit("          /provider list|add <name> [url]|rm <name> (profiles)");
            S.screen.commit("          /login <name> [key] · /model <provider>/<name>");
            S.screen.commit("          /tools (tool registry) · /mcp [resources|prompts <srv>] · /cwd [path]");
            S.screen.commit("          /approve ask|allowlist|off (tool gate)");
            S.screen.commit("          /compact (summarize history; context relief)");
            S.screen.commit("          /models (list & pick) · /log <path>|off|default");
            S.screen.commit(
                "          /log info|debug|trace (capture level; /debug on|all|off alias)");
            S.screen.commit(
                "          /config · /sessions · /session <id|prefix>|new · /sysinfo on|off");
            S.screen.commit("          /version (build identity: version · commit · built)");
            S.screen.commit("          /sidebar on|off (tree + changed files)");
            S.screen.commit("          /preview <path> (show a file; .md renders)");
            S.screen.commit("          /prompt <server>/<name> [k=v...] (mcp prompt)");
            S.screen.commit("          /exit");
            S.screen.commit("Each of /provider /login /model saves on its own;");
            S.screen.commit("configure them in any order, any number of sittings.");
            S.screen.commit("prompt: ?=unconfigured/unverified · ~=checking · >=ok");
            S.screen.commit("        !=key rejected · x=provider unreachable");
            S.screen.commit("Enter sends · Enter queues while streaming");
            return true;
        }
        if (line.rfind("/login", 0) == 0) {
            // v6 (design 012 §5.2): a first token naming a configured
            // profile targets that profile's key; anything else is the key
            // itself for the active profile. v7: with no profiles the
            // command auto-creates default (design 013 §5.4).
            const auto arg = arg_of(line);
            S.ensure_default_profile();
            std::string target = S.config.active_provider;
            std::string_view key = arg;
            if (!arg.empty()) {
                const auto space = arg.find(' ');
                const auto first = space == std::string_view::npos ? arg : arg.substr(0, space);
                if (S.config.find_provider(first) != nullptr) {
                    target = std::string(first);
                    key = space == std::string_view::npos ? std::string_view() : arg.substr(space);
                    while (!key.empty() && key.front() == ' ')
                        key.remove_prefix(1);
                }
            }
            const std::string label = " (" + target + ")";
            if (key.empty()) {
                S.secret_input = true; // masked composer; next Enter sets key
                // The profile name pins the target even if the active
                // profile changes before Enter lands.
                S.secret_field = "api_key:" + target;
                S.screen.commit("[input] API key" + label +
                              " (hidden); Enter confirms, empty Enter cancels");
            } else {
                auto* profile = S.config.find_provider(target);
                profile->api_key = std::string(key);
                S.auth_probes[target] = AuthProbe{}; // verdict belongs to the old key
                if (target == S.config.current.name)
                    S.auth = AuthProbe{};
                S.config.refresh_current();
                S.save_and_report(true);
                S.maybe_autofetch_models();
            }
            return true;
        }
        if (line.rfind("/models", 0) == 0) {
            // Listed before /model so the shared prefix resolves here first.
            // v6: any complete profile (or the unnamed route) can be listed;
            // the fetch aggregates every ready target.
            bool any_ready = !S.config.current.base_url.empty() && !S.config.current.api_key.empty();
            for (const auto& p : S.config.providers)
                any_ready = any_ready || (!p.base_url.empty() && !p.api_key.empty());
            if (!any_ready) {
                S.screen.commit("[error] model listing needs base_url + api_key ·" +
                              config_next_hint(S.config));
            } else if (S.models_running) {
                S.screen.commit("[info] already listing models, wait for the list");
            } else {
                S.screen.commit("[info] listing models from provider...");
                S.start_models_fetch(true); // force: explicit re-list bypasses cache
            }
            return true;
        }
        if (line.rfind("/model", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const std::string shown =
                    S.config.current.name.empty()
                        ? (S.config.current.model.empty() ? "(not set)" : S.config.current.model)
                        : S.config.current.name + "/" + S.config.current.model;
                S.screen.commit("model=" + shown + "; usage /model <name> | <provider>/<name>" +
                              (S.config.current.base_url.empty() || S.config.current.api_key.empty()
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
                    S.config.find_provider(provider) != nullptr) {
                    S.select_model(provider, id, "slash");
                    return true;
                }
                std::vector<std::string> owners;
                for (std::size_t i = 0; i < S.model_choice_targets.size(); ++i) {
                    const auto& target = S.model_choice_targets[i];
                    if (target.second == ref &&
                        std::find(owners.begin(), owners.end(), target.first) == owners.end())
                        owners.push_back(target.first);
                }
                if (owners.size() > 1) {
                    std::string names;
                    for (const auto& owner : owners)
                        names += (names.empty() ? "" : ", ") +
                                 (owner.empty() ? "(none)" : owner);
                    S.screen.commit("[error] model '" + ref +
                                  "' exists on several providers: " + names);
                    S.screen.commit("        pick one with /model <provider>/" + ref);
                } else if (owners.size() == 1) {
                    S.select_model(owners.front(), ref, "slash");
                } else {
                    // Not in the fetched list: literal set on the active
                    // route (legacy behavior -- the id may simply not be
                    // listed).
                    S.select_model("", ref, "slash");
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
                if (S.config.providers.empty()) {
                    S.screen.commit("providers: none · usage /provider add <name> [url]");
                } else {
                    S.screen.commit("providers (" + std::to_string(S.config.providers.size()) + "):");
                    for (const auto& p : S.config.providers)
                        S.screen.commit("  " + p.name + " · " +
                                      (p.base_url.empty() ? "(url not set)" : p.base_url) +
                                      " · key=" + (p.api_key.empty() ? "(not set)" : "***set***") +
                                      (p.name == S.config.active_provider ? "  <- active" : ""));
                }
                S.screen.commit("usage /provider <url> · /provider add <name> [url] · "
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
                    S.screen.commit("[error] " + err);
                    return true;
                }
                if (auto* existing = S.config.find_provider(name)) {
                    // Upsert (same rule as MCP): URL replaces when given;
                    // key/model/metadata survive an add without a URL.
                    if (!url.empty()) {
                        existing->base_url = std::string(url);
                        S.auth_probes.erase(name);
                        if (name == S.config.current.name) {
                            S.auth = AuthProbe{};
                            S.config.refresh_current();
                        }
                    }
                    S.screen.commit("[ok] provider '" + name + "' updated" +
                                  (url.empty() ? "" : " · " + std::string(url)));
                } else {
                    S.config.providers.push_back(std::move(probe));
                    // v7 (design 013 §5.4): the first profile auto-activates
                    // so the table never exists without a selection.
                    if (S.config.providers.size() == 1) {
                        S.config.active_provider = name;
                        S.config.refresh_current();
                        S.switch_active_profile();
                    }
                    S.screen.commit("[ok] provider '" + name + "' added" +
                                  (url.empty() ? "" : " · " + std::string(url)));
                }
                const auto* added = S.config.find_provider(name);
                if (added->base_url.empty())
                    S.screen.commit("     next: /provider add " + name +
                                  " <url> · then /login " + name);
                else if (added->api_key.empty())
                    S.screen.commit("     next: /login " + name + " (key stays hidden)");
                S.logger->event("provider_add", {{"provider", name}});
                S.save_and_report(true);
                S.maybe_autofetch_models();
            } else if (arg.rfind("rm", 0) == 0 && (arg.size() == 2 || arg[2] == ' ')) {
                auto rest = arg.substr(2);
                while (!rest.empty() && rest.front() == ' ')
                    rest.remove_prefix(1);
                const std::string name(rest);
                auto it = std::find_if(S.config.providers.begin(), S.config.providers.end(),
                                       [&](const ProviderProfile& p) { return p.name == name; });
                if (name.empty() || it == S.config.providers.end()) {
                    S.screen.commit("[error] unknown provider '" + name +
                                  "' · /provider lists the names");
                    return true;
                }
                const bool was_active = name == S.config.active_provider;
                S.config.providers.erase(it);
                S.auth_probes.erase(name);
                if (was_active) {
                    // v7 (design 013 §5.4, Q2): fall back to the first
                    // remaining profile; with none left the setup state
                    // returns. The removed profile's credentials go with it.
                    if (!S.config.providers.empty())
                        S.config.activate_provider(S.config.providers.front().name);
                    else
                        S.config.active_provider.clear();
                    S.config.refresh_current();
                    S.switch_active_profile();
                    S.screen.commit(S.config.providers.empty()
                                      ? "[info] active profile removed; no profiles left · "
                                        "/provider add <name> or /login to start over"
                                      : "[info] active profile removed; switched to '" +
                                            S.config.active_provider + "'");
                }
                S.logger->event("provider_rm", {{"provider", name}});
                S.save_and_report(true);
                S.screen.commit("[ok] provider '" + name + "' removed");
            } else {
                // /provider <url>: edit the active profile's URL (v7: no
                // unnamed top level; auto-default when no profile exists).
                S.ensure_default_profile();
                ProviderProfile probe;
                probe.name = S.config.active_provider;
                probe.base_url = std::string(arg);
                std::string err;
                if (!probe.valid(&err)) {
                    S.screen.commit("[error] " + err);
                    return true;
                }
                auto* active = S.config.find_provider(S.config.active_provider);
                active->base_url = probe.base_url;
                while (!active->base_url.empty() && active->base_url.back() == '/')
                    active->base_url.pop_back();
                S.auth_probes.erase(active->name);
                if (active->name == S.config.current.name)
                    S.auth = AuthProbe{}; // a different endpoint cannot inherit it
                S.config.refresh_current();
                S.save_and_report(true);
                S.maybe_autofetch_models();
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
                S.screen.commit(S.logger->enabled() ? "logging on: " + S.config.log_file
                                                : "logging off; usage /log <path> or /log off");
                S.screen.commit("level=" + S.config.log_level +
                              " (session-only) · usage /log info|debug|trace");
            } else if (arg == "info" || arg == "debug" || arg == "trace") {
                // v4.8.1: the level is session-scoped -- set in memory,
                // never saved (persistence would resurrect a debug switch
                // across sessions; the maintainer rule for this knob).
                S.config.log_level = std::string(arg);
                if (S.config.log_level == "info") {
                    S.screen.commit("[ok] log level info (this session) · metadata log only, "
                                  "no raw dumps");
                } else {
                    S.screen.commit("[ok] log level " + S.config.log_level + " (this session)" +
                                  (S.config.log_level == "debug"
                                       ? " · raw chat traffic"
                                       : " · chat + truncated aux traffic") +
                                  " -> " + S.config.to_provider().debug_log_path);
                }
            } else if (arg == "off") {
                S.config.log_file = "off"; // sentinel: explicit disable
                S.logger = S.make_logger();
                S.save_and_report();
                S.screen.commit("[ok] logging disabled");
            } else if (arg == "default") {
                S.config.log_file.clear(); // empty -> /tmp/mini-agent-<uid>.log
                S.logger = S.make_logger();
                S.save_and_report();
                const auto shown = Logger::resolve_path(S.config);
                S.screen.commit("[ok] logging back to default: " +
                              (shown ? *shown : std::string("(none)")));
            } else {
                S.config.log_file = std::string(arg);
                S.logger = S.make_logger();
                S.save_and_report();
                if (S.logger->enabled()) {
                    S.screen.commit("[ok] logging to: " + S.config.log_file);
                } else {
                    S.screen.commit("[warn] log path not writable, logging stays off");
                }
            }
            return true;
        }
        if (line.rfind("/compact", 0) == 0) {
            // 015 (design 015 §4C): manual history compaction. Idle-gated
            // like /session; the summary request runs on its own worker.
            if (S.streaming || S.compact_running || S.approval_pending()) {
                S.screen.commit("[info] /compact needs an idle turn · wait or Ctrl-C");
                return true;
            }
            std::size_t count = 0;
            for (const auto& m : S.session.chat.messages()) {
                if (m.role != Role::System)
                    ++count;
            }
            if (count < 2) {
                S.screen.commit("[info] nothing to compact (fewer than 2 messages)");
                return true;
            }
            // 017: if the whole history fits the retained-tail budget there
            // is nothing to summarize -- compacting would only add an empty
            // checkpoint (same guard family as "cannot reclaim, do not run").
            const std::size_t keep_from = find_retained_tail(S.session.chat.messages(), 20000);
            std::size_t region_non_system = 0;
            for (std::size_t i = 0; i < keep_from; ++i) {
                if (S.session.chat.messages()[i].role != Role::System)
                    ++region_non_system;
            }
            if (region_non_system == 0) {
                S.screen.commit("[info] nothing to compact (history fits the retained tail budget)");
                return true;
            }
            if (!S.config.complete()) {
                S.screen.commit("[error] LLM not configured ·" + config_next_hint(S.config));
                return true;
            }
            // 017 (design 017 §2.2): verbatim backup BEFORE any replacement;
            // a failed backup aborts the compact (history stays untouched).
            if (!S.temp_mode) {
                std::string backup_err;
                if (!S.store.save_backup(S.session, &backup_err)) {
                    S.screen.commit("[error] compact backup failed: " + backup_err +
                                  " (history unchanged)");
                    return true;
                }
            }
            const AppConfig snapshot = S.config; // thread boundary
            const Chat hchat = S.session.chat; // thread boundary
            S.compact_result = CompactResult{};
            S.compact_done = false;
            S.compact_running = true;
            S.abort_requested = false;
            S.logger->event("compact", {{"status", "start"},
                                      {"before", std::to_string(hchat.messages().size())}});
            S.screen.commit("[info] compacting " + std::to_string(hchat.messages().size()) +
                          " messages (recent turns kept verbatim, original backed up; "
                          "Ctrl-C/Esc cancels)...");
            S.compact_worker = std::thread([&, snapshot, hchat] {
                S.compact_result =
                    compact_history(snapshot, hchat, [&] { return S.abort_requested.load(); });
                S.compact_done = true;
            });
            return true;
        }
        if (line == "/config") {
            S.screen.commit("base_url=" + (S.config.current.base_url.empty() ? "(not set)" : S.config.current.base_url));
            S.screen.commit("model=" + (S.config.current.model.empty() ? "(not set)" : S.config.current.model));
            S.screen.commit("api_key=" +
                          std::string(S.config.current.api_key.empty() ? "(not set)" : "***set***"));
            S.screen.commit("login=" + std::string(auth_log_token(S.auth)));
            // v6 (design 012): profile table size + active selection.
            S.screen.commit("providers=" + std::to_string(S.config.providers.size()) + " active=" +
                          (S.config.active_provider.empty() ? "(no provider)" : S.config.active_provider) +
                          (S.config.env_override_active ? " · env override in effect" : ""));
            S.screen.commit("log_file=" + (S.config.log_file.empty() ? "(off)" : S.config.log_file));
            S.screen.commit(std::string("log_level=") + S.config.log_level +
                          " (session-only, not saved)");
            // v4 fields (design 010).
            S.screen.commit("context_window=" +
                          (S.config.current.context_window > 0
                               ? format_token_count(S.config.current.context_window) + " (" +
                                     std::to_string(S.config.current.context_window) + ")"
                               : std::string("(not set)")));
            S.screen.commit("reasoning_effort=" + (S.config.current.reasoning_effort.empty()
                                                     ? std::string("(not set)")
                                                     : S.config.current.reasoning_effort));
            S.screen.commit("config file: " + default_config_path().string() +
                          " (env overrides file)");
            return true;
        }
        if (line.rfind("/sidebar", 0) == 0) {
            // v4.13: right sidebar with the workspace tree + changed
            // files. Session-only display state (same rule as /sysinfo).
            const auto arg = arg_of(line);
            if (arg.empty()) {
                S.screen.commit(std::string("sidebar=") + (S.show_sidebar ? "on" : "off") +
                              " · usage /sidebar on|off");
            } else if (arg == "on") {
                S.show_sidebar = true;
                S.start_sidebar();
                S.screen.commit("[ok] sidebar on (this session) · tree + git changes");
            } else if (arg == "off") {
                S.show_sidebar = false;
                S.sidebar_focus = false; // v4.14: no focus survives the panel
                S.screen.commit("[ok] sidebar off (this session)");
            } else {
                S.screen.commit("[error] /sidebar expects on|off");
            }
            return true;
        }
        if (line.rfind("/sysinfo", 0) == 0) {
            // v4.12: bottom system-info row toggle (user@host · cwd ·
            // clock). Session-only like the log level -- display state,
            // not a durable preference.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                S.screen.commit(std::string("sysinfo=") + (S.show_sysinfo ? "on" : "off") +
                              " · usage /sysinfo on|off");
            } else if (arg == "on") {
                S.show_sysinfo = true;
                S.screen.commit("[ok] system info row on (session-only)");
            } else if (arg == "off") {
                S.show_sysinfo = false;
                S.screen.commit("[ok] system info row off (session-only)");
            } else {
                S.screen.commit("[error] /sysinfo expects on|off");
            }
            return true;
        }
        if (line.rfind("/version", 0) == 0) {
            // 0.8.0: runtime build identity, same three lines as the CLI
            // `version` subcommand. Read-only; works in any state.
            S.screen.commit("mini-agent v" MINI_AGENT_VERSION);
            S.screen.commit("commit " MINI_AGENT_COMMIT);
            S.screen.commit("built " MINI_AGENT_BUILD_TIME);
            return true;
        }
        if (line == "/sessions") {
            std::string err;
            const auto ids = S.store.list(&err);
            S.screen.commit(ids.empty() ? "sessions: (empty)" : "sessions:");
            for (const auto& id : ids)
                S.screen.commit("  " + id + (id == S.session.meta.id ? "  <- current" : ""));
            S.screen.commit("switch in place: /session <id or prefix> · /session new");
            return true;
        }
        if (line.rfind("/session", 0) == 0 && line != "/sessions") {
            // v4.9: switch or fork the live session without exiting. Same
            // idle-gated swap discipline as /cwd (worker join, registry
            // rebuild); the old session is saved and unlocked, the target
            // is loaded and locked. A failed lock/load rolls back.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                S.screen.commit("session=" + S.session.meta.id +
                              " · usage /session <id or prefix> | /session new");
                return true;
            }
            if (S.streaming || S.turn_done.load() || S.compact_running) {
                // The turn-completion handler writes session.chat after
                // joining; swapping mid-fold would land the old turn in
                // the new session. Require idle (Ctrl-C interrupts).
                S.screen.commit("[info] session switch needs an idle turn · wait or Ctrl-C");
                return true;
            }
            std::string target_id = std::string(arg);
            const bool fresh = target_id == "new";
            if (fresh) {
                target_id = mini_agent::generate_session_id();
            } else if (!mini_agent::session_id_is_safe(target_id)) {
                S.screen.commit("[error] unsafe session id");
                return true;
            } else {
                // v4.13: prefix match (maintainer request). The typed word
                // resolves against stored ids; one hit replaces it, several
                // list candidates, none falls through so `load` reports the
                // usual "no such session" error.
                S.session_ids_stale = true;
                S.refresh_session_ids();
                const auto m = mini_agent::match_session_ref(target_id, S.session_ids);
                if (m.exact) {
                    target_id = m.id;
                } else if (!m.candidates.empty()) {
                    S.screen.commit("session " + target_id + " is ambiguous:");
                    for (const auto& id : m.candidates)
                        S.screen.commit("  " + id + (id == S.session.meta.id ? "  <- current" : ""));
                    S.screen.commit("type more characters, or Tab to cycle /session <id>");
                    return true;
                }
            }
            if (target_id == S.session.meta.id) {
                S.screen.commit("[info] already on session " + target_id);
                return true;
            }
            std::string err;
            // Persist the outgoing session first; a save failure is a warn
            // only (the switch proceeds; the old file keeps last-turn state).
            // v4.15: a temp session has no file and never gets one, so the
            // pre-switch save is skipped (its chat is dropped on switch-out).
            if (!S.temp_mode && !S.store.save(S.session, &err)) {
                S.screen.commit("[warn] save before switch failed: " + err);
            }
            Session next;
            std::optional<std::string> next_workspace;
            if (fresh) {
                next.meta.id = target_id;
                next.meta.model = S.config.current.model;
                next_workspace = S.config.workspace.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>(S.config.workspace);
                next.meta.workspace = next_workspace;
                next.meta.provider =
                    S.config.current.name.empty()
                        ? std::optional<std::string>()
                        : std::optional<std::string>(S.config.current.name);
                std::string mcp_hint;
                for (const auto& server : S.config.mcp_servers) {
                    if (!mcp_hint.empty())
                        mcp_hint += ", ";
                    mcp_hint += server.name;
                }
                mini_agent::inject_system_prompt(next.chat, S.config.workspace, S.config.current.model,
                                                 S.config.system_prompt, mcp_hint);
            } else {
                auto loaded = S.store.load(target_id, &err);
                if (!loaded) {
                    S.screen.commit("[error] load failed: " + sanitize_display(err));
                    return true;
                }
                next = std::move(*loaded);
            }
            if (!S.store.try_lock(target_id, &err)) {
                S.screen.commit("[error] session busy: " + sanitize_display(err));
                return true;
            }
            S.store.unlock(S.session.meta.id);
            S.session = std::move(next);
            S.active_session_id = S.session.meta.id;
            // v4.15: `/session new` from a temp session stays temp (the new
            // chat is equally ephemeral); resuming a saved id ends temp mode
            // -- from here on the target behaves like a normal session.
            S.temp_mode = S.temp_mode && fresh;
            // v3 workspace precedence on switch (same as resume in cmd_run):
            // the session's own stamp rebinds config; else keep current.
            if (S.session.meta.workspace && !S.session.meta.workspace->empty()) {
                S.config.workspace = *S.session.meta.workspace;
                S.session.chat.update_system_workspace(S.config.workspace);
                std::string note;
                S.tool_registry = build_tool_registry(S.config, &note);
            }
            S.config_snapshot = S.config; // worker turn snapshot refresh (same as /cwd)
            S.reset_usage_readings();   // usage belongs to the new session
            // Re-seed ↑/↓ history from the new chat (same rule as launch).
            S.history.clear();
            for (const auto& m : S.session.chat.messages())
                if (m.role == Role::User && !m.content.empty() && m.content.front() != '/')
                    S.history.push_back(m.content);
            S.hist_pos = S.history.size();
            S.history_draft.clear();
            S.logger->event("session_switch", {{"id", S.session.meta.id},
                                             {"fresh", fresh ? "new" : "resume"},
                                             {"temp", S.temp_mode ? "on" : "off"}});
            S.screen.commit(std::string("[ok] ") + (fresh ? "new" : "resumed") +
                          (S.temp_mode ? " temp" : "") + " session " + S.session.meta.id + " · " +
                          std::to_string(S.session.chat.messages().size()) + " message(s)");
            return true;
        }
        if (line.rfind("/context", 0) == 0) {
            // v4 (design 010): model context window for the status-bar
            // ctx% reading. Config is authoritative -- no probing.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                const auto inputs = S.context_reading();
                std::string shown = S.config.current.context_window > 0
                                        ? format_token_count(S.config.current.context_window) + " (" +
                                              std::to_string(S.config.current.context_window) + ")"
                                        : std::string("(not set)");
                // v4.7: when unset, surface the catalog reading (the same
                // value the status bar silently uses) so the user can see
                // and confirm it before it becomes config-authoritative.
                if (S.config.current.context_window <= 0 && inputs.context_window > 0)
                    shown += " · catalog: " + format_token_count(inputs.context_window) + " (" +
                             std::to_string(inputs.context_window) + ") · set it: /context " +
                             std::to_string(inputs.context_window);
                S.screen.commit("context_window=" + shown +
                              "; usage /context <tokens|128K|1M> | /context probe | /context off");
                S.screen.commit(
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
                S.screen.commit("breakdown (estimated): system ~" +
                              format_token_count(inputs.breakdown.system) + " · conversation ~" +
                              format_token_count(inputs.breakdown.conversation) +
                              " · tool output ~" +
                              format_token_count(inputs.breakdown.tool_output));
                if (inputs.context_exceeded)
                    S.screen.commit("[warn] last request overflowed the window · reading "
                                  "synthesized until the next successful turn");
                if (const long long threshold = pressure_threshold(inputs.context_window);
                    threshold > 0)
                    S.screen.commit("pressure: /compact hint at >= " + format_token_count(threshold) +
                                  " tokens (window - max(15%, 16k))");
            } else if (arg == "off" || arg == "0") {
                S.active_profile_for_edit()->context_window = 0;
                S.config.refresh_current(); // v7: per-profile metadata
                S.save_and_report();
                S.screen.commit("[ok] context window cleared · status shows tokens, no %");
            } else if (arg == "probe") {
                // v4.3 (design 010 §13): explicit, user-triggered cheap
                // probe -- one oversized-max_tokens request whose rejection
                // carries the gateway's authoritative numbers. Never
                // automatic: it costs a request and the message text is
                // not an API contract, so the maintainer decides when.
                if (S.probe_running) {
                    S.screen.commit("[info] probe already in flight, wait for the result");
                    return true;
                }
                if (S.config.current.base_url.empty() || S.config.current.api_key.empty() || S.config.current.model.empty()) {
                    S.screen.commit("[error] probe needs base_url + api_key + model ·" +
                                  config_next_hint(S.config));
                    return true;
                }
                if (S.probe_worker.joinable())
                    S.probe_worker.join();
                S.probe_running = true;
                S.probe_done = false;
                S.probe_result = ProbeResult{};
                S.logger->event("ctx_probe", {{"status", "start"},
                                            {"base_url", Logger::redact_url(S.config.current.base_url)},
                                            {"model", S.config.current.model}});
                const AppConfig snapshot = S.config; // thread boundary
                S.probe_worker = std::thread([&, snapshot] {
                    S.probe_result = probe_window(snapshot);
                    S.probe_done = true;
                });
                S.screen.commit("[info] probing context window (one oversized request)...");
            } else {
                // §21: decimal K/M suffixes (128K=128000, 1M=1000000);
                // plain digits stay exact. 0 = invalid, nothing written.
                const long long parsed_window = parse_token_count(arg);
                if (parsed_window <= 0) {
                    S.screen.commit("[error] /context expects a positive token count "
                                  "(e.g. 131072, 128K, 1M)");
                    return true;
                }
                S.active_profile_for_edit()->context_window = parsed_window;
                S.config.refresh_current(); // v7: per-profile metadata
                S.save_and_report();
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
                S.screen.commit("reasoning_effort=" +
                              (S.config.current.reasoning_effort.empty() ? std::string("(not set)")
                                                               : S.config.current.reasoning_effort) +
                              "; usage /reasoning <level> or /reasoning off");
                // v4.2 (§11): cached capabilities for the current model ->
                // numbered picker over the gateway's supported levels (the
                // same interaction as /models). A literal value still works
                // for models without advertised metadata.
                const auto caps = S.caps_for_model();
                if (!caps.reasoning_efforts.empty()) {
                    S.close_preview_pane(); // 018: menu takes over the live region
                    S.reasoning_choices = caps.reasoning_efforts;
                    S.reasoning_menu_open = true;
                    for (const auto& row : reasoning_menu_rows(
                             S.reasoning_choices, S.config.current.reasoning_effort,
                             caps.reasoning_default_effort, caps.reasoning_mandatory))
                        S.screen.commit(row);
                } else if (caps.supports_reasoning) {
                    S.screen.commit("[info] gateway marks this model as reasoning-capable "
                                  "but lists no levels; set one explicitly");
                }
            } else if (arg == "off") {
                S.active_profile_for_edit()->reasoning_effort.clear();
                S.config.refresh_current(); // v7: per-profile metadata
                S.save_and_report();
                S.screen.commit("[ok] reasoning effort cleared · requests omit the field");
            } else {
                S.active_profile_for_edit()->reasoning_effort = std::string(arg);
                S.config.refresh_current(); // v7: per-profile metadata
                S.save_and_report();
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
                S.config.log_level = level; // session-scoped (v4.8.1): no save
                if (S.config.log_level == "info") {
                    S.screen.commit(std::string(ok_text) + " · metadata log only");
                } else {
                    S.screen.commit(std::string(ok_text) + " -> " +
                                  S.config.to_provider().debug_log_path);
                }
            };
            if (arg.empty()) {
                const auto pc = S.config.to_provider();
                S.screen.commit(
                    std::string("debug=") + S.config.log_level +
                    (S.config.capture_chat() ? " · file: " + pc.debug_log_path : std::string()) +
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
                S.screen.commit("[error] /debug expects on|all|off|info|debug|trace");
            }
            return true;
        }
        if (line.rfind("/approve", 0) == 0) {
            // v3.1 runtime approval switch: /approve [ask|allowlist|off].
            // Worker must be idle (policy is snapshotted per turn), same
            // join discipline as /cwd and /tools.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                S.screen.commit("approval=" + S.approval_policy +
                              " · /approve ask|allowlist|off · 'a' key also "
                              "allows a tool name for this session");
                if (S.approval_policy == "allowlist") {
                    std::string joined;
                    for (const auto& name : S.config.tool_allowlist) {
                        if (!joined.empty())
                            joined += ", ";
                        joined += name;
                    }
                    S.screen.commit("allowlist: " +
                                  (joined.empty() ? std::string("(empty)") : joined));
                }
                return true;
            }
            const std::string want(arg);
            if (want != "ask" && want != "allowlist" && want != "off") {
                S.screen.commit("[error] /approve expects ask|allowlist|off");
                return true;
            }
            if (S.worker.joinable())
                S.worker.join();
            S.approval_policy = want;
            S.config.tool_approval = want;
            S.config_snapshot = S.config; // worker turn snapshot refresh (same as /cwd)
            S.save_and_report(false);
            if (want == "off" && !S.tool_registry->empty())
                S.screen.commit("[warn] tool_approval=off: tools run WITHOUT asking"
                              " (no sandbox: run_bash executes at your user's "
                              "privileges; git push stays human-only by tool "
                              "absence, but shell can still reach remotes)");
            else if (want == "allowlist" && S.config.tool_allowlist.empty())
                S.screen.commit("[info] allowlist mode with an EMPTY list: every tool "
                              "still asks; add names via tool_allowlist in config.json");
            else
                S.screen.commit("[ok] approval=" + S.approval_policy + " saved");
            S.logger->event("approval_switch", {{"policy", S.approval_policy}});
            return true;
        }
        if (line.rfind("/cwd", 0) == 0) {
            // v3 (design 009 5.4): query or rebind the session workspace.
            const auto arg = arg_of(line);
            if (arg.empty()) {
                S.screen.commit("workspace: " +
                              sanitize_display(S.config.workspace.empty()
                                                   ? std::filesystem::current_path().string()
                                                   : S.config.workspace) +
                              " · /cwd <path> to rebind (file+git tools and run_bash)");
                return true;
            }
            const std::filesystem::path target(arg);
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(target, ec);
            if (ec || !std::filesystem::is_directory(canonical, ec)) {
                S.screen.commit("[error] not a directory: " + sanitize_display(arg));
                return true;
            }
            if (S.worker.joinable())
                S.worker.join(); // registry swap needs the worker idle (008 thread rule)
            S.config.workspace = canonical.string();
            S.session.meta.workspace = S.config.workspace;
            S.session.chat.update_system_workspace(S.config.workspace);
            std::string note;
            S.tool_registry = build_tool_registry(S.config, &note);
            S.config_snapshot = S.config;
            S.sidebar_browse.clear(); // 018: browser resets to the new workspace
            S.start_sidebar(); // v4.13: tree belongs to the new boundary
            S.screen.commit("[ok] workspace=" + sanitize_display(S.config.workspace) +
                          " · tools rebound · " +
                          (S.config.workspace == std::filesystem::current_path().string()
                               ? "(matches launch cwd)"
                               : "run_bash now starts inside it"));
            return true;
        }
        if (line.rfind("/preview", 0) == 0) {
            const auto arg = arg_of(line);
            if (arg.empty()) {
                S.screen.commit("usage: /preview <path> (workspace-bounded; .md renders)");
                return true;
            }
            S.open_preview_pane(std::string(arg));
            return true;
        }
        if (line == "/tools") {
            // Rebuild the registry so fresh server configs apply without
            // a restart; old clients die with their transports (RAII).
            if (S.worker.joinable())
                S.worker.join();
            S.config_snapshot = S.config;
            std::string note;
            S.tool_registry = build_tool_registry(S.config, &note);
            std::vector<std::string> rendered;
            for (const auto& definition : S.tool_registry->list()) {
                auto first_line = definition.description.find('\n');
                rendered.push_back(definition.name + " \u00b7 " + definition.source + " \u00b7 " +
                                   sanitize_display(definition.description.substr(0, first_line)));
            }
            for (const auto& row : tool_registry_rows(rendered))
                S.screen.commit(row);
            if (!note.empty())
                S.screen.commit("[info] " + note + " (lazy start on first call)");
            S.screen.commit("approval=" + S.approval_policy);
            return true;
        }
        if (line.rfind("/prompt", 0) == 0 &&
            (line.size() == 7 || line[7] == ' ' || line[7] == '\t')) {
            // M2a T3 (design 022 §5.1): render a server prompt template
            // and inject its messages as user-role content. Explicit user
            // action -- no model round involved, so no approval gate.
            if (S.worker.joinable())
                S.worker.join(); // registry access needs the worker idle (008 thread rule)
            const auto arg = arg_of(line);
            const auto space = arg.find_first_of(" \t");
            const std::string target(arg.substr(0, space));
            const std::string_view rest =
                space == std::string_view::npos ? std::string_view() : arg.substr(space + 1);
            const auto slash = target.find('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 == target.size()) {
                S.screen.commit("[error] usage: /prompt <server>/<name> [key=value ...]");
                return true;
            }
            const std::string server = target.substr(0, slash);
            const std::string name = target.substr(slash + 1);
            McpClient* client = S.tool_registry
                                    ? dynamic_cast<McpClient*>(S.tool_registry->find_by_source(server))
                                    : nullptr;
            if (!client) {
                S.screen.commit("[error] no mcp server '" + sanitize_display(server) +
                                "' (check config.json mcp_servers, then /tools)");
                return true;
            }
            // k=v pairs become string-valued prompt arguments; quotes are
            // literal (shell-style stripping is not the TUI contract).
            nlohmann::json arguments = nlohmann::json::object();
            std::size_t pos = 0;
            bool args_ok = true;
            while (pos < rest.size()) {
                while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == '\t'))
                    ++pos;
                if (pos >= rest.size())
                    break;
                auto end = rest.find_first_of(" \t", pos);
                if (end == std::string_view::npos)
                    end = rest.size();
                const auto token = rest.substr(pos, end - pos);
                const auto eq = token.find('=');
                if (eq == std::string_view::npos || eq == 0) {
                    S.screen.commit("[error] prompt arguments must be key=value pairs");
                    args_ok = false;
                    break;
                }
                arguments[std::string(token.substr(0, eq))] = std::string(token.substr(eq + 1));
                pos = end;
            }
            if (!args_ok)
                return true;
            std::string error;
            const std::string result = client->get_prompt(name, arguments.dump(), &error);
            if (!error.empty()) {
                S.screen.commit("[error] prompt get failed: " + sanitize_display(error));
                return true;
            }
            const auto parsed = nlohmann::json::parse(result, nullptr, false);
            if (!parsed.is_object()) {
                S.screen.commit("[error] prompt returned no messages");
                return true;
            }
            const auto messages = parsed.find("messages");
            if (messages == parsed.end() || !messages->is_array() || messages->empty()) {
                S.screen.commit("[error] prompt returned no messages");
                return true;
            }
            int injected = 0;
            int downgraded = 0;
            for (const auto& message : *messages) {
                if (!message.is_object())
                    continue;
                const auto content = message.find("content");
                if (content == message.end() || !content->is_object())
                    continue;
                const auto type = content->find("type");
                std::string text;
                if (type != content->end() && type->is_string() &&
                    type->get_ref<const std::string&>() == "text") {
                    if (auto t = content->find("text"); t != content->end() && t->is_string())
                        text = t->get<std::string>();
                } else {
                    // T3 downgrade: the chat seam is text-only; non-text
                    // blocks become explicit placeholders, never dropped
                    // silently (design 022 §5.1).
                    text = "[unsupported content: " +
                           (type != content->end() && type->is_string()
                                ? type->get<std::string>()
                                : std::string("unknown")) +
                           "]";
                    ++downgraded;
                }
                if (text.empty())
                    continue;
                // Prompt role is template metadata; injection is always
                // user-role (an MCP server must not forge assistant turns).
                Message injected_message;
                injected_message.role = Role::User;
                injected_message.content = std::move(text);
                S.session.chat.add(std::move(injected_message));
                ++injected;
            }
            if (injected == 0) {
                S.screen.commit("[error] prompt returned no usable messages");
                return true;
            }
            S.screen.commit("[info] prompt " + sanitize_display(target) + ": injected " +
                            std::to_string(injected) + " user message(s)" +
                            (downgraded > 0 ? " (" + std::to_string(downgraded) +
                                                  " non-text block(s) downgraded to placeholders)"
                                            : ""));
            std::string save_error;
            if (!S.temp_mode && !S.store.save(S.session, &save_error))
                S.screen.commit("[warn] session save failed: " + save_error);
            return true;
        }
        if (line.rfind("/mcp", 0) == 0 &&
            (line.size() == 4 || line[4] == ' ' || line[4] == '\t')) {
            // M2a T3 (design 022 §5.1): `/mcp` = passive status (never
            // starts a server, lazy contract 008 §5.5);
            // `/mcp resources|prompts <server>` = explicit listing (starts
            // the named server on demand, like /tools does for tools).
            const auto arg = arg_of(line);
            const auto space = arg.find_first_of(" \t");
            const std::string sub(arg.substr(0, space));
            const std::string server_name(space == std::string_view::npos
                                              ? std::string()
                                              : std::string(arg.substr(space + 1)));
            if (!sub.empty() && sub != "resources" && sub != "prompts") {
                S.screen.commit("[error] usage: /mcp [resources|prompts <server>]");
                return true;
            }
            if (!sub.empty()) {
                if (server_name.empty()) {
                    S.screen.commit("[error] usage: /mcp " + sub + " <server>");
                    return true;
                }
                if (S.worker.joinable())
                    S.worker.join(); // registry access needs the worker idle
                auto* client = S.tool_registry
                                   ? dynamic_cast<McpClient*>(
                                         S.tool_registry->find_by_source(server_name))
                                   : nullptr;
                if (!client) {
                    S.screen.commit("[error] no mcp server '" + sanitize_display(server_name) +
                                    "' (check config.json mcp_servers, then /tools)");
                    return true;
                }
                std::string error;
                const bool resources = sub == "resources";
                const std::string listed = resources ? client->list_resources(&error)
                                                     : client->list_prompts(&error);
                if (!error.empty()) {
                    S.screen.commit("[error] " + sub + " list failed: " + sanitize_display(error));
                    return true;
                }
                const auto parsed = nlohmann::json::parse(listed, nullptr, false);
                if (!parsed.is_array() || parsed.empty()) {
                    S.screen.commit(sub + " \u00b7 " + sanitize_display(server_name) + ": (none)");
                    return true;
                }
                for (const auto& entry : parsed) {
                    if (!entry.is_object())
                        continue;
                    std::string row;
                    if (resources) {
                        const auto uri = entry.find("uri");
                        const auto name = entry.find("name");
                        row = (uri != entry.end() && uri->is_string() ? uri->get<std::string>()
                                                                      : std::string("?"));
                        if (name != entry.end() && name->is_string())
                            row += " \u00b7 " + name->get<std::string>();
                    } else {
                        const auto name = entry.find("name");
                        row = name != entry.end() && name->is_string()
                                  ? name->get<std::string>()
                                  : std::string("?");
                        const auto desc = entry.find("description");
                        if (desc != entry.end() && desc->is_string()) {
                            const auto& text = desc->get_ref<const std::string&>();
                            row += " \u00b7 " + text.substr(0, text.find('\n'));
                        }
                    }
                    S.screen.commit("  " + sanitize_display(row));
                }
                if (!resources)
                    S.screen.commit("run one: /prompt " + sanitize_display(server_name) +
                                    "/<name> [k=v...]");
                return true;
            }
            if (S.config.mcp_servers.empty()) {
                S.screen.commit("mcp servers: (none) \u00b7 add mcp_servers to config.json");
                return true;
            }
            if (S.worker.joinable())
                S.worker.join(); // client state reads need the worker idle
            for (const auto& server : S.config.mcp_servers) {
                std::string surface;
                // PASSIVE lookup: find_by_source never calls list(), so
                // status lines cannot spawn/handshake a server (lazy
                // contract, design 008 §5.5). Counts are listed only for
                // ALREADY-initialized clients (metadata cache; a running
                // server may be queried, an unstarted one never is).
                auto* client = S.tool_registry
                                   ? dynamic_cast<McpClient*>(
                                         S.tool_registry->find_by_source(server.name))
                                   : nullptr;
                if (client && client->initialized()) {
                    std::string error;
                    if (client->has_resources()) {
                        const auto listed =
                            nlohmann::json::parse(client->list_resources(&error), nullptr, false);
                        surface += " \u00b7 resources=" +
                                   (listed.is_array() ? std::to_string(listed.size())
                                                      : std::string("?"));
                    }
                    if (client->has_prompts()) {
                        const auto listed =
                            nlohmann::json::parse(client->list_prompts(&error), nullptr, false);
                        surface += " \u00b7 prompts=" +
                                   (listed.is_array() ? std::to_string(listed.size())
                                                      : std::string("?"));
                    }
                } else if (client && !client->alive()) {
                    surface = " \u00b7 down";
                }
                if (surface.empty())
                    surface = " \u00b7 not started (lazy)";
                S.screen.commit("  " + server.name + " \u00b7 " + server.transport + "\u2192" +
                              (server.transport == "http" ? Logger::redact_url(server.url)
                                                          : sanitize_display(server.command)) +
                              surface);
            }
            S.screen.commit("list details: /mcp resources|prompts <server> \u00b7 config edits: /tools");
            return true;
        }
        if (line == "/exit")
            return false; // caller exits
        if (!line.empty() && line.front() == '/') {
            S.screen.commit("[error] unknown command " + std::string(line) + " (/help)");
            return true;
        }
        return false;
}
} // namespace mini_agent::tui
