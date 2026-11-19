#pragma once
// TUI input helpers (019 P4). Templates over run_loop context.
#include <filesystem>
#include <string>
#include "mini_agent/tui.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/session.hpp"
namespace mini_agent::tui {
template<class Ctx> void hist_up_impl(Ctx& S) {
        if (S.hist_pos == 0)
            return;
        if (S.hist_pos == S.history.size())
            S.history_draft = S.composer; // leaving the draft slot: keep it
        S.composer = S.history[--S.hist_pos];
        S.cursor = S.composer.size();
}
template<class Ctx> void hist_down_impl(Ctx& S) {
        if (S.hist_pos == S.history.size())
            return;
        if (++S.hist_pos == S.history.size()) {
            S.composer = S.history_draft;
            S.history_draft.clear();
        } else {
            S.composer = S.history[S.hist_pos];
        }
        S.cursor = S.composer.size();
}
template<class Ctx> void mark_edited_impl(Ctx& S) {
        if (S.hist_pos != S.history.size())
            S.history_draft = S.composer;
}
template<class Ctx> bool insert_sidebar_selection_impl(Ctx& S) {
        if (S.sidebar_sel >= S.sidebar_result.paths.size() || S.sidebar_result.paths[S.sidebar_sel].empty())
            return false;
        const std::string& rawp = S.sidebar_result.paths[S.sidebar_sel];
        std::string rel = rawp;
        if (!rel.empty() && rel.front() == '/') {
            std::error_code rec;
            const auto ws = S.config.workspace.empty() ? std::filesystem::current_path()
                                                     : std::filesystem::path(S.config.workspace);
            const auto r = std::filesystem::relative(std::filesystem::path(rel), ws, rec);
            if (!rec)
                rel = r.string();
        }
        utf8_insert(S.composer, S.cursor, rel);
        mark_edited_impl(S);
        S.screen.commit("[sidebar] inserted: " + sanitize_display(rel) +
                      " · edit freely, Enter sends as usual");
        S.sidebar_focus = false;
        return true;
}
} // namespace mini_agent::tui
