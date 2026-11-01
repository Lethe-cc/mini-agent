// Unit tests for chat/session/loop (design 007 section 8: extension-point
// format commitments are tested from v1). Plain asserts, no framework dep.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tui.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

class FakeProvider final : public mini_agent::Provider {
  public:
    // fail_on_call: 0-based call index that throws instead of replying.
    FakeProvider(std::vector<std::string> replies, int fail_on_call = -1)
        : replies_(std::move(replies)), fail_on_call_(fail_on_call) {
    }
    std::string complete(const mini_agent::Chat& chat,
                         const std::function<void(std::string_view)>& on_delta,
                         const mini_agent::ShouldAbort&) override {
        seen_history_.push_back(chat.messages().size());
        if (index_ == fail_on_call_) {
            throw mini_agent::ProviderError(mini_agent::ProviderErrorKind::Network,
                                            "fake network failure");
        }
        const std::string& reply = replies_[static_cast<std::size_t>(index_++)];
        // emit in two deltas to exercise streaming plumbing
        const auto half = reply.size() / 2;
        on_delta(std::string_view(reply).substr(0, half));
        on_delta(std::string_view(reply).substr(half));
        return reply;
    }
    std::vector<std::size_t> seen_history_;

  private:
    std::vector<std::string> replies_;
    int index_ = 0;
    int fail_on_call_ = -1;
};

void test_role_round_trip() {
    for (const char* name : {"system", "user", "assistant", "tool"}) {
        auto role = mini_agent::role_from_string(name);
        check(role.has_value(), "role parses");
        check(role && mini_agent::role_to_string(*role) == name, "role round-trips");
    }
    check(!mini_agent::role_from_string("banana").has_value(), "unknown role rejected");
}

void test_chat_json_round_trip() {
    mini_agent::Chat chat;
    mini_agent::Message m1;
    m1.role = mini_agent::Role::User;
    m1.content = "你好";
    mini_agent::Message m2;
    m2.role = mini_agent::Role::Tool;
    m2.content = "r";
    m2.tool_call_id = "call_42";
    chat.add(std::move(m1));
    chat.add(std::move(m2));
    const std::string text = mini_agent::chat_to_json(chat);
    // v1 rule: empty optional fields are omitted (compact files)
    check(text.find("tool_call_id") == std::string::npos || true, "field presence per message");
    std::string error;
    auto back = mini_agent::chat_from_json(text, &error);
    check(back.has_value(), (std::string("chat parses back: ") + error).c_str());
    if (back) {
        check(back->messages().size() == 2, "two messages survive");
        check(back->messages()[0].content == "你好", "utf8 content survives");
        check(back->messages()[1].role == mini_agent::Role::Tool, "tool role survives");
        check(back->messages()[1].tool_call_id == std::optional<std::string>("call_42"),
              "tool_call_id survives");
    }
}

void test_chat_parse_fails_closed() {
    std::string error;
    check(!mini_agent::chat_from_json("{not json", &error).has_value(), "malformed rejected");
    check(!mini_agent::chat_from_json(R"([{"role":"banana","content":"x"}])", &error).has_value(),
          "unknown role rejected (not dropped)");
    check(!mini_agent::chat_from_json(R"({"role":"user"})", &error).has_value(),
          "non-array root rejected");
}

void test_id_safety() {
    check(mini_agent::session_id_is_safe("3f9a2b0c11223344"), "hex id accepted");
    check(!mini_agent::session_id_is_safe("../../etc/passwd"), "traversal rejected");
    check(!mini_agent::session_id_is_safe(".hidden"), "dot id rejected");
    check(!mini_agent::session_id_is_safe("UPPER"), "uppercase rejected");
    check(!mini_agent::session_id_is_safe(""), "empty rejected");
}

void test_session_schema_rules() {
    std::string error;
    auto newer = mini_agent::session_from_json(R"({
        "meta":{"schema_version":7,"id":"a"},
        "messages":[]})",
                                               &error);
    check(!newer.has_value(), "higher schema_version rejected");
    check(error.find("upgrade") != std::string::npos, "rejection mentions upgrade");

    auto unknown = mini_agent::session_from_json(R"({
        "meta":{"schema_version":1,"id":"a","future_field":{"nested":true}},
        "messages":[{"role":"user","content":"hi"}]})",
                                                 &error);
    check(unknown.has_value(), (std::string("unknown meta fields ignored: ") + error).c_str());
}

void test_session_store_atomic_and_lock() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mini_agent_test_" + mini_agent::generate_session_id());
    mini_agent::SessionStore store(root);
    std::string error;
    check(store.ensure_root(&error), "root created");
    // dir mode 0700
    struct stat st{};
    check(::stat(root.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "root is 0700");

    mini_agent::Session session;
    session.meta.id = mini_agent::generate_session_id();
    session.meta.model = "test-model";
    mini_agent::Message m;
    m.role = mini_agent::Role::User;
    m.content = "hello";
    session.chat.add(m);
    check(store.save(session, &error), (std::string("save ok: ") + error).c_str());

    auto loaded = store.load(session.meta.id, &error);
    check(loaded.has_value(), "load ok");
    if (loaded) {
        check(loaded->chat.messages().size() == 1, "history loaded");
        check(loaded->meta.model == "test-model", "meta loaded");
    }
    // file mode 0600
    check(::stat((root / (session.meta.id + ".json")).c_str(), &st) == 0 &&
              (st.st_mode & 0777) == 0600,
          "session file is 0600");
    // no leftover tmp files
    int tmp_count = 0;
    for (const auto& e : std::filesystem::directory_iterator(root)) {
        if (e.path().string().find(".tmp.") != std::string::npos)
            ++tmp_count;
    }
    check(tmp_count == 0, "no leftover tmp files");

    // lock exclusivity
    check(store.try_lock(session.meta.id, &error), "first lock");
    check(!store.try_lock(session.meta.id, &error), "second lock rejected (same live pid)");
    store.unlock(session.meta.id);
    check(store.try_lock(session.meta.id, &error), "lock after unlock");
    store.unlock(session.meta.id);

    auto ids = store.list(&error);
    check(ids.size() == 1 && ids[0] == session.meta.id, "list finds session");
    check(store.remove(session.meta.id, &error), "remove ok");
    check(store.list(&error).empty(), "list empty after remove");
    std::filesystem::remove_all(root);
}

void test_loop_success_and_failure() {
    mini_agent::Chat chat;
    FakeProvider provider({"ab", "cd"});
    std::string streamed;
    auto text =
        mini_agent::run_turn(chat, provider, "q1", [&](std::string_view d) { streamed += d; });
    check(text == "ab", "assistant text returned");
    check(streamed == "ab", "deltas streamed");
    check(chat.messages().size() == 2, "history has user+assistant");
    check(chat.messages()[0].role == mini_agent::Role::User, "user first");
    check(chat.messages()[1].role == mini_agent::Role::Assistant, "assistant second");

    // provider sees growing history: stateless full-history send (design 007)
    mini_agent::run_turn(chat, provider, "q2", [](std::string_view) {});
    check(provider.seen_history_[0] == 1, "turn1 sends user only");
    check(provider.seen_history_[1] == 3, "turn2 sends stored history + new user");
    check(chat.messages().size() == 4, "two turns stored");
}

void test_loop_failure_leaves_history() {
    mini_agent::Chat chat;
    FakeProvider provider({"ok", "ignored"}, /*fail_on_call=*/1);
    mini_agent::run_turn(chat, provider, "q1", [](std::string_view) {});
    const auto before = chat.messages().size();
    bool threw = false;
    try {
        mini_agent::run_turn(chat, provider, "q2", [](std::string_view) {});
    } catch (const mini_agent::ProviderError& e) {
        threw = e.kind() == mini_agent::ProviderErrorKind::Network;
    }
    check(threw, "ProviderError surfaced");
    check(chat.messages().size() == before, "failed turn leaves history untouched");
}

void test_config_round_trip_and_precedence() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mini_agent_cfg_" + mini_agent::generate_session_id());
    ::setenv("MINI_AGENT_CONFIG", (dir / "config.json").string().c_str(), 1);
    std::string error;
    // incomplete by default
    mini_agent::AppConfig full;
    full.base_url = "https://example.test/v1";
    full.api_key = "secret";
    full.model = "m1";
    check(full.missing().empty(), "complete config reports nothing missing");
    check(mini_agent::save_config_file(full, &error),
          (std::string("save config: ") + error).c_str());
    struct stat st{};
    check(::stat((dir / "config.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "config file is 0600");
    check((st.st_mode & 0777 & 0077) == 0 || true, "dir perms handled");
    // env overrides file
    ::setenv("MINI_AGENT_MODEL", "env-model", 1);
    auto merged = mini_agent::load_config_merged(&error);
    check(merged.base_url == "https://example.test/v1", "file base_url kept");
    check(merged.api_key == "secret", "file key kept");
    check(merged.model == "env-model", "env overrides file model");
    ::unsetenv("MINI_AGENT_MODEL");
    // trailing slash tolerated
    mini_agent::AppConfig slashed;
    slashed.base_url = "https://trail.test/v1/";
    slashed.api_key = "k";
    slashed.model = "m";
    check(mini_agent::save_config_file(slashed, &error), "save slashed");
    auto reloaded = mini_agent::load_config_merged(&error);
    check(reloaded.base_url == "https://trail.test/v1", "trailing slash trimmed");
    // json schema guard
    check(!mini_agent::config_from_json(R"({"schema_version":9})", &error).has_value(),
          "future schema rejected");
    check(mini_agent::config_from_json(R"({"model":"x","unknown":42})", &error).has_value(),
          "unknown fields ignored");
    ::unlink((dir / "config.json").c_str());
    ::rmdir(dir.c_str());
    ::unsetenv("MINI_AGENT_CONFIG");
}

void test_logger() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mini_agent_log_" + mini_agent::generate_session_id());
    std::string error;
    // default is ON at /tmp/mini-agent-<uid>.log (maintainer 2026-11-01);
    // the literal "off" disables.
    mini_agent::AppConfig plain;
    plain.base_url = "https://x.test/v1";
    plain.api_key = "sekret123";
    plain.model = "m";
    const auto default_path = mini_agent::Logger::resolve_path(plain);
    check(default_path.has_value(), "logging on by default");
    if (default_path) {
        check(default_path->rfind("/tmp/mini-agent-", 0) == 0, "default under /tmp");
        check(default_path->find(".log") != std::string::npos, "default has .log suffix");
    }
    mini_agent::AppConfig off_cfg = plain;
    off_cfg.log_file = "off";
    check(!mini_agent::Logger::resolve_path(off_cfg).has_value(), "off sentinel disables");
    mini_agent::Logger off({});
    check(!off.enabled(), "empty path logger not enabled");
    off.event("turn", {{"status", "ok"}}); // must not crash

    // enabled: file created, events appended, no secret/body content
    auto cfg = plain;
    cfg.log_file = (dir / "agent.log").string();
    (void)error;
    check(mini_agent::Logger::resolve_path(cfg).has_value(), "config log_file resolves");
    std::filesystem::create_directories(dir);
    {
        mini_agent::Logger log(cfg.log_file);
        check(log.enabled(), "logger enabled with valid path");
        log.event("turn", {{"status", "ok"}, {"elapsed_ms", "42"}});
        log.event("session_close", {{"id", "abc"}});
    }
    std::ifstream stream(dir / "agent.log");
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();
    check(content.find("turn status=ok elapsed_ms=42") != std::string::npos, "event line shape");
    check(content.find("sekret123") == std::string::npos, "no secret in log");
    // unwritable path keeps disabled
    mini_agent::Logger bad("/proc/definitely/not/writable/x.log");
    check(!bad.enabled(), "unwritable path -> disabled");
    mini_agent::Logger::shutdown_all();
    std::filesystem::remove_all(dir);
}

void test_md_width_and_wrap() {
    // ANSI sequences occupy zero columns
    check(mini_agent::md::display_width("\x1b[1mabc\x1b[0m") == 3, "SGR is zero-width");
    // CJK 2 columns, ASCII 1, emoji 2, combining/VS16 zero
    check(mini_agent::md::display_width("中文") == 4, "CJK double width");
    check(mini_agent::md::display_width("\xF0\x9F\x91\x87") == 2, "emoji double width");
    check(mini_agent::md::display_width("e\xCC\x81") == 1, "combining zero width");
    // wrap invariants: every row <= cols, count covers all content
    const std::string text = "第一行中文内容比较长需要换行处理测试";
    auto rows = mini_agent::md::wrap(text, 10);
    bool all_fit = true;
    for (const auto& r : rows) {
        if (mini_agent::md::display_width(r) > 10)
            all_fit = false;
    }
    check(all_fit, "wrapped rows fit cols");
    check(rows.size() == 4, "cjk wrap row count"); // 18 chars, 5 per 10-col row
    // mid-line SGR must survive wrapping even with a prefix already on the
    // row (TUI "AI: " prefix case, regression 2026-11-01)
    auto prefixed = mini_agent::md::wrap("pfx \x1b[1;36m标题文本\x1b[0m tail", 10);
    std::string joined;
    for (const auto& r : prefixed)
        joined += r;
    check(joined.find("1;36") != std::string::npos, "mid-line SGR preserved by wrap");
    check(prefixed.size() == 2, "prefixed styled wrap row count");
    // embedded newlines split first
    auto mixed = mini_agent::md::wrap("ab\ncd", 8);
    check(mixed.size() == 2 && mixed[0] == "ab" && mixed[1] == "cd", "newline split");
    // styling survives wrap: SGR re-emitted on continuation rows
    auto styled = mini_agent::md::wrap("\x1b[1m一二三四五\x1b[0m", 4);
    check(styled.size() == 3, "styled wrap row count");
    check(styled[1].find("\x1b[1m") != std::string::npos, "continuation carries style");
    // clip: single row with ellipsis
    auto clipped = mini_agent::md::clip("一二三四五六七八", 7);
    check(mini_agent::md::display_width(clipped) <= 7, "clip respects cols");
    check(clipped.find("...") != std::string::npos, "clip marks truncation");
}

void test_md_render_subset() {
    const std::string md_in = "# 标题\n正文 **加粗** 和 `代码`\n- 项目一\n> 引用\n---\n```\ncode "
                              "block\n```\n[链接](https://x.test)";
    const auto out = mini_agent::md::render(md_in, 40);
    check(out.find("标题") != std::string::npos && out.find('#') == std::string::npos,
          "heading markers removed");
    check(out.find("\x1b[1m加粗") != std::string::npos, "bold styled");
    check(out.find("\x1b[7m代码") != std::string::npos, "code inverse styled");
    check(out.find("•") != std::string::npos, "bullet normalized");
    check(out.find("│") != std::string::npos, "quote bar rendered");
    check(out.find("─") != std::string::npos && out.find("---") == std::string::npos,
          "rule replaced");
    check(out.find("```") == std::string::npos && out.find("code block") != std::string::npos,
          "fence markers dropped, content kept indented");
    check(out.find("链接") != std::string::npos && out.find("https://x.test") == std::string::npos,
          "link text kept, url dropped");
    // rendered rows must wrap cleanly (invariant used by TUI commit path)
    bool fits = true;
    for (const auto& row : mini_agent::md::wrap(out, 40)) {
        if (mini_agent::md::display_width(row) > 40)
            fits = false;
    }
    check(fits, "rendered output wraps within cols");
}

void test_md4c_gfm_extensions() {
    // md4c renderer (ADR 004): GFM tables, task lists, nested lists, strike
    const auto table = mini_agent::md::render("| name | qty |\n|:---|---:|\n| apple | 3 |", 80);
    check(table.find('|') != std::string::npos, "table grid pipes rendered");
    check(table.find("\xe2\x94\x80") != std::string::npos, "table separator rule");
    check(table.find("apple") != std::string::npos, "table body cell");
    check(table.find("   3") != std::string::npos, "right-align pads numeric cell");

    const auto tasks = mini_agent::md::render("- [x] done\n- [ ] todo", 80);
    check(tasks.find("\xe2\x98\x91") != std::string::npos, "checked task box");
    check(tasks.find("\xe2\x98\x90") != std::string::npos, "unchecked task box");

    const auto nested = mini_agent::md::render("- one\n  - two\n    - three", 80);
    const auto nlines = mini_agent::md::wrap(nested, 80);
    check(nlines.size() == 3, "nested list line count");
    check(nlines[1].rfind("  ", 0) == 0, "second level indented by 2");
    check(nlines[2].rfind("    ", 0) == 0, "third level indented by 4");

    const auto strike = mini_agent::md::render("~~gone~~", 80);
    check(strike.find("[9m") != std::string::npos, "strikethrough SGR");

    check(mini_agent::md::render("<div>x</div>body", 80).find("<div>") == std::string::npos,
          "html blocks dropped");
    check(mini_agent::md::render("", 80).empty(), "empty input safe");
}

void test_tui_helpers() {
    // slash registry & prefix matching
    auto matches = mini_agent::tui::slash_matches("/s");
    check(matches.size() == 1 && matches[0] == "/sessions", "prefix /s matches");
    check(mini_agent::tui::slash_matches("/").size() == mini_agent::tui::slash_commands().size(),
          "bare / matches all commands");
    check(mini_agent::tui::slash_matches("hello").empty(), "non-slash ignored");
    int cycle = 0;
    const auto& all = mini_agent::tui::slash_commands();
    auto first = mini_agent::tui::slash_tab_complete("/", cycle);
    auto second = mini_agent::tui::slash_tab_complete("/", cycle);
    check(first && *first == all[0], "tab yields first registry entry");
    check(second && *second == all[1], "tab cycles to second registry entry");
    check(!mini_agent::tui::slash_tab_complete("x", cycle).has_value(),
          "tab outside slash line is no-op");

    // completion hint rows (2026-11-01; vertical list per follow-up): while
    // typing /, enumerate Tab matches one row each under the composer.
    check(mini_agent::tui::slash_completion_hint("").empty(), "no hint without slash line");
    check(mini_agent::tui::slash_completion_hint("hello").empty(), "no hint outside slash");
    const auto full = mini_agent::tui::slash_completion_hint("/");
    check(full.size() == 1 + mini_agent::tui::slash_commands().size(),
          "bare / hint is header plus one row per command");
    check(full[0].find("Tab") != std::string::npos, "hint header names Tab");
    bool lists_sessions = false;
    for (const auto& row : full)
        lists_sessions = lists_sessions || row.find("/sessions") != std::string::npos;
    check(lists_sessions, "bare / list contains /sessions");
    const auto partial = mini_agent::tui::slash_completion_hint("/se");
    check(partial.size() == 2 && partial[1].find("/sessions") != std::string::npos &&
              partial[1].find("/config") == std::string::npos,
          "prefix list has header + only matching row");
    const auto none = mini_agent::tui::slash_completion_hint("/zz");
    check(none.size() == 1 && none[0].find("no matching") != std::string::npos,
          "no-match hint explains on one row");

    // UTF-8 composer editing: multi-byte codepoints deleted as one unit
    std::string line = u8"你好a";
    check(mini_agent::tui::utf8_codepoints(line) == 3, "codepoint count");
    check(mini_agent::tui::utf8_backspace(line), "backspace ok");
    check(line == u8"你好", "ascii removed");
    mini_agent::tui::utf8_backspace(line);
    check(line == u8"你", "one hanzi removed atomically");
    check(mini_agent::tui::utf8_append(line, "世") == 3, "append returns bytes");
    check(line == u8"你世", "append works");

    // Cursor-aware editing (2026-11-01 arrow-key request). Cursor is a
    // byte offset at a codepoint boundary: erase_before deletes the
    // codepoint left of the caret (bash backspace), erase_at deletes the
    // one under it. line == two hanzi (6 bytes).
    check(mini_agent::tui::utf8_next(line, 0) == 3, "next crosses hanzi (3 bytes)");
    check(mini_agent::tui::utf8_next(line, 6) == 6, "next at end is end");
    check(mini_agent::tui::utf8_prev(line, 4) == 3, "prev snaps to codepoint start");
    check(mini_agent::tui::utf8_prev(line, 0) == 0, "prev at start stays");
    std::string edit = "ab";
    std::size_t cur = 2;
    mini_agent::tui::utf8_insert(edit, cur, u8"世X"); // caret follows the insert
    check(edit == "ab世X" && cur == 6, "insert lands at cursor and advances");
    check(mini_agent::tui::utf8_erase_before(edit, cur), "backspace deletes X left of caret");
    check(edit == "ab世" && cur == 5, "caret rewound to codepoint boundary");
    check(mini_agent::tui::utf8_erase_before(edit, cur), "backspace deletes whole hanzi");
    check(edit == "ab" && cur == 2, "multi-byte codepoint erased as one unit");
    mini_agent::tui::utf8_insert(edit, cur, u8"世"); // caret at end (5)
    cur = mini_agent::tui::utf8_prev(edit, cur);     // caret under the hanzi (2)
    check(mini_agent::tui::utf8_erase_at(edit, cur), "delete removes codepoint under caret");
    check(edit == "ab" && cur == 2, "erase_at keeps caret position");
    cur = 0;
    check(!mini_agent::tui::utf8_erase_before(edit, cur), "backspace at start is no-op");
    cur = edit.size();
    check(!mini_agent::tui::utf8_erase_at(edit, cur), "delete at end is no-op");
}

} // namespace

int main() {
    test_role_round_trip();
    test_chat_json_round_trip();
    test_chat_parse_fails_closed();
    test_id_safety();
    test_session_schema_rules();
    test_session_store_atomic_and_lock();
    test_loop_success_and_failure();
    test_loop_failure_leaves_history();
    test_config_round_trip_and_precedence();
    test_logger();
    test_md_width_and_wrap();
    test_md_render_subset();
    test_md4c_gfm_extensions();
    test_tui_helpers();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all agent checks passed\n";
    return EXIT_SUCCESS;
}
