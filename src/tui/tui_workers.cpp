// TUI background workers (design 019 P4). Bodies moved verbatim from the
// run_loop thread closures; parameterized over snapshot/abort.
#include "tui_workers.hpp"

#include "mini_agent/context.hpp"

namespace mini_agent::tui {

ModelsResult fetch_models(const AppConfig& snapshot, const ModelTargets& targets) {
    ModelsResult r;
    for (const auto& [name, creds] : targets) {
        ModelsResult::Part part;
        part.name = name;
        // Bounded wait: a hung /models must not stall the TUI for
        // the full chat timeout budget (design 007 section 6).
        ProviderConfig pc = snapshot.to_provider();
        pc.base_url = creds.first;
        pc.api_key = creds.second;
        pc.connect_timeout_seconds = 5;
        pc.total_timeout_seconds = pc.total_timeout_seconds < 15 ? pc.total_timeout_seconds : 15;
        try {
            // One provider instance per target: the lazy CURL handle
            // is NOT thread-safe, and sequential GETs keep the
            // failure isolation simple (design 012 §5.3).
            OpenAiProvider provider(std::move(pc));
            // v4.2: one GET carries both the id list and the optional
            // capability metadata (OpenRouter-style gateways).
            part.caps = provider.list_model_capabilities();
            for (const auto& cap : part.caps)
                part.ids.push_back(cap.id);
            part.ok = true;
        } catch (const ProviderError& e) {
            // parse_models_meta_json shares parse_models_json's fail-
            // closed root/data guards, so a Protocol error here would
            // fail the id-only path identically -- no fallback needed.
            part.error = e.what();
            part.http_status = e.http_status();
            part.network_error = e.kind() == ProviderErrorKind::Network;
        } catch (const std::exception& e) {
            part.error = e.what();
        }
        r.parts.push_back(std::move(part));
    }
    // Legacy fields mirror the EFFECTIVE target (current.name, which
    // env may pin to another profile) so the existing folds keep
    // single-profile semantics.
    for (const auto& part : r.parts) {
        if (part.name != snapshot.current.name)
            continue;
        r.ok = part.ok;
        r.ids = part.ids;
        r.caps = part.caps;
        r.error = part.error;
        r.http_status = part.http_status;
        r.network_error = part.network_error;
        break;
    }
    return r;
}

ProbeResult probe_window(const AppConfig& snapshot) {
    ProbeResult r;
    try {
        ProviderConfig pc = snapshot.to_provider();
        pc.total_timeout_seconds = pc.total_timeout_seconds < 30 ? pc.total_timeout_seconds : 30;
        OpenAiProvider provider(pc);
        r.outcome = provider.probe_context_window();
    } catch (const ProviderError& e) {
        r.error = e.what();
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

CompactResult compact_history(const AppConfig& snapshot, const Chat& history,
                              const std::function<bool()>& abort) {
    CompactResult r;
    try {
        // 017: cut the retained tail at a turn boundary, prune
        // old verbose tool output in the summarize region, and
        // carry an old marked summary as previous-summary.
        const auto& msgs = history.messages();
        const std::size_t cut = find_retained_tail(msgs, 20000);
        r.tail.assign(msgs.begin() + static_cast<std::ptrdiff_t>(cut), msgs.end());
        Chat region;
        std::string previous;
        const std::string kMarker = "[conversation summary]\n";
        for (std::size_t i = 0; i < cut; ++i) {
            if (msgs[i].role == Role::User && msgs[i].content.rfind(kMarker, 0) == 0) {
                previous = msgs[i].content.substr(kMarker.size());
                continue;
            }
            region.add(msgs[i]);
        }
        const auto pruned = prune_tool_outputs(region.messages(), 4);
        r.pruned = pruned.pruned;
        Chat pruned_chat;
        pruned_chat.replace(pruned.messages);
        OpenAiProvider provider(snapshot.to_provider());
        const Chat request = build_compact_request(pruned_chat, previous);
        // No tools, no UI deltas: the summary is not a turn.
        r.summary = provider.complete(request, [](std::string_view) {}, [&] { return abort(); });
        // 017: one NUDGE retry when the template was ignored.
        if (!abort() && !summary_has_template(r.summary)) {
            r.nudged = true;
            Chat nudge = request;
            nudge.add(Message{Role::User,
                              "Template reminder: return the summary as text using "
                              "the exact section headings (## Goal, ## Progress, "
                              "## Next Steps, ...). Do not call tools.",
                              {},
                              {}});
            r.summary = provider.complete(nudge, [](std::string_view) {}, [&] { return abort(); });
            if (!summary_has_template(r.summary))
                r.error = "summary did not follow the required template";
        }
    } catch (const ProviderError& e) {
        r.error = e.what();
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    if (abort())
        r.aborted = true;
    return r;
}

} // namespace mini_agent::tui
