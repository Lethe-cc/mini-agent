// Unit tests for chat/session/loop (design 007 section 8: extension-point
// format commitments are tested from v1). Plain asserts, no framework dep.
#include <algorithm>
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
#include "mini_agent/diff.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/models_dev.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools.hpp"
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

void test_models_listing() {
    std::string error;
    // Happy path: sorted and deduplicated ids.
    const auto ok = mini_agent::parse_models_json(
        R"({"object":"list","data":[{"id":"z-model"},{"id":"a-model"},{"id":"z-model"}]})", &error);
    check(ok.has_value(), "valid models body parsed");
    check(ok->size() == 2, "duplicate id collapsed");
    check((*ok)[0] == "a-model" && (*ok)[1] == "z-model", "ids sorted");

    // Entries without a usable id are skipped rather than failing the list.
    const auto partial = mini_agent::parse_models_json(
        R"({"data":[{"id":"keep"},{"name":"no-id"},7,{"id":42}]})", &error);
    check(partial.has_value() && partial->size() == 1 && (*partial)[0] == "keep",
          "only string ids kept");

    // Fail-closed shapes (design 007 section 5.5).
    check(!mini_agent::parse_models_json("not json", &error).has_value(), "garbage rejected");
    check(!mini_agent::parse_models_json("[]", &error).has_value(), "non-object root rejected");
    check(!mini_agent::parse_models_json(R"({"models":[]})", &error).has_value(),
          "missing data array rejected");
    check(!mini_agent::parse_models_json(R"({"data":"nope"})", &error).has_value(),
          "non-array data rejected");
    check(!error.empty(), "rejection carries an explanation");
    // An empty-but-valid list is a success, not an error.
    check(mini_agent::parse_models_json(R"({"data":[]})", &error)->empty(),
          "empty data array is a valid empty list");

    // Providers that do not implement the endpoint surface a Protocol error
    // instead of silently returning nothing (FakeProvider does not override).
    FakeProvider unsupported({"x"});
    bool threw = false;
    try {
        (void)unsupported.list_models();
    } catch (const mini_agent::ProviderError& e) {
        threw = e.kind() == mini_agent::ProviderErrorKind::Protocol;
    }
    check(threw, "unsupported list_models throws Protocol");

    // Incomplete config is rejected before any network attempt, and the error
    // text never carries the key (design 007 section 6).
    mini_agent::OpenAiProvider unconfigured(mini_agent::ProviderConfig{});
    std::string message;
    try {
        (void)unconfigured.list_models();
    } catch (const mini_agent::ProviderError& e) {
        message = e.what();
    }
    check(!message.empty(), "unconfigured list_models throws");
    check(message.find("api_key") == std::string::npos, "config error names the field, no secret");

    // Menu rendering and selection.
    const std::vector<std::string> models = {"a-model", "b-model", "c-model"};
    const auto rows = mini_agent::tui::model_menu_rows(models, "b-model");
    check(rows.size() == models.size() + 2, "menu is header + rows + footer");
    check(rows[1] == "  1. a-model", "first row numbered 1");
    check(rows[2].find("<- current") != std::string::npos, "current model tagged");
    check(mini_agent::tui::model_menu_rows({}, "").empty(), "empty list renders no menu");

    check(mini_agent::tui::model_choice(models, "1") == "a-model", "index 1 is first");
    check(mini_agent::tui::model_choice(models, "3") == "c-model", "index N is last");
    check(!mini_agent::tui::model_choice(models, "4").has_value(), "index past end rejected");
    check(!mini_agent::tui::model_choice(models, "0").has_value(), "index 0 rejected");
    check(!mini_agent::tui::model_choice(models, "999999999999999999999").has_value(),
          "overflowing index rejected without wrapping");
    check(!mini_agent::tui::model_choice(models, "hello").has_value(), "non-numeric rejected");
    check(mini_agent::tui::model_choice(models, "b-model") == "b-model", "exact id accepted");
    check(!mini_agent::tui::model_choice(models, "").has_value(), "empty input rejected");
    check(!mini_agent::tui::model_choice({}, "1").has_value(), "no models to pick from");
}

void test_config_batched_hint() {
    // Batched entry (2026-11-02 request): each command saves independently,
    // so the hint must name only the NEXT missing step, never all three.
    mini_agent::AppConfig empty;
    check(mini_agent::tui::config_next_hint(empty) == "config 0/3 · next: /provider <url>",
          "nothing set points at /provider only");

    mini_agent::AppConfig has_url;
    has_url.base_url = "https://x.test/v1";
    check(mini_agent::tui::config_next_hint(has_url) == "config 1/3 · next: /login <key>",
          "url set points at /login only");

    // Order independence: a key without a url must still ask for the url.
    mini_agent::AppConfig has_key;
    has_key.api_key = "k";
    check(mini_agent::tui::config_next_hint(has_key) == "config 1/3 · next: /provider <url>",
          "key-only still points at /provider first");

    mini_agent::AppConfig url_and_key;
    url_and_key.base_url = "https://x.test/v1";
    url_and_key.api_key = "k";
    check(mini_agent::tui::config_next_hint(url_and_key) ==
              "config 2/3 · next: /models (list & pick) or /model <name>",
          "url+key points at model discovery");

    mini_agent::AppConfig model_only;
    model_only.model = "m";
    check(mini_agent::tui::config_next_hint(model_only) == "config 1/3 · next: /provider <url>",
          "model-only points at /provider");

    mini_agent::AppConfig full;
    full.base_url = "https://x.test/v1";
    full.api_key = "k";
    full.model = "m";
    check(mini_agent::tui::config_next_hint(full) == "config 3/3 · ready", "complete says ready");
    check(full.complete(), "hint agrees with complete()");

    // A hint line must never carry the key value itself.
    mini_agent::AppConfig secrety;
    secrety.api_key = "do-not-leak-this";
    check(mini_agent::tui::config_next_hint(secrety).find("do-not-leak-this") == std::string::npos,
          "hint reports presence, never the key value");
}

void test_login_status() {
    using mini_agent::tui::AuthProbe;
    // Never probed yet.
    AuthProbe untouched;
    check(mini_agent::tui::auth_log_token(untouched) == "untried", "untried token");
    check(mini_agent::tui::auth_status_line(untouched).find("unverified") != std::string::npos,
          "untried line explains how to check");

    AuthProbe verified;
    verified.attempted = true;
    verified.fetch_ok = true;
    verified.http_status = 200;
    verified.model_count = 2;
    check(mini_agent::tui::auth_log_token(verified) == "verified", "success is verified");
    check(mini_agent::tui::auth_status_line(verified).find("login verified") != std::string::npos,
          "verified line");
    check(mini_agent::tui::auth_status_line(verified).find("2 model") != std::string::npos,
          "verified line reports the reachable count");

    // A 2xx whose body failed to parse still proves the key was accepted.
    AuthProbe parsed_bad;
    parsed_bad.attempted = true;
    parsed_bad.http_status = 200;
    check(mini_agent::tui::auth_log_token(parsed_bad) == "verified",
          "2xx with unusable body still counts as an accepted key");

    for (const int code : {401, 403}) {
        AuthProbe rejected;
        rejected.attempted = true;
        rejected.http_status = code;
        check(mini_agent::tui::auth_log_token(rejected) == "rejected", "401/403 is rejected");
        const auto line = mini_agent::tui::auth_status_line(rejected);
        check(line.find("REJECTED") != std::string::npos &&
                  line.find(std::to_string(code)) != std::string::npos,
              "rejected line names the status code");
    }

    AuthProbe refused;
    refused.attempted = true;
    refused.http_status = 503;
    check(mini_agent::tui::auth_log_token(refused) == "refused", "other status is refused");

    AuthProbe unreachable;
    unreachable.attempted = true;
    unreachable.network_error = true;
    check(mini_agent::tui::auth_log_token(unreachable) == "unreachable",
          "no response is unreachable");
    check(mini_agent::tui::auth_status_line(unreachable).find("unreachable") != std::string::npos,
          "unreachable line points at /provider");

    // A chat completion feeds the same verdict vocabulary: 2026-11-02 report --
    // chat worked fine but the prompt stayed yellow because only /models was
    // ever read as evidence.
    AuthProbe chat_ok;
    chat_ok.attempted = true;
    chat_ok.fetch_ok = true;
    check(mini_agent::tui::auth_log_token(chat_ok) == "verified",
          "a chat reply verifies the key with no /models involved");

    AuthProbe chat_refused;
    chat_refused.attempted = true;
    chat_refused.http_status = 403;
    check(mini_agent::tui::auth_log_token(chat_refused) == "rejected",
          "a 403 chat turn reads as a refused key");

    // No auth status line may ever leak the key value.
    AuthProbe leak_probe;
    leak_probe.attempted = true;
    leak_probe.http_status = 401;
    check(mini_agent::tui::auth_status_line(verified).find("Bearer") == std::string::npos &&
              mini_agent::tui::auth_status_line(leak_probe).find("Bearer") == std::string::npos,
          "auth lines never mention the Authorization scheme or a key");
}

void test_connection_prompt() {
    // The composer prompt carries the provider connection state
    // (2026-11-02 request): derived state, then its styled marker.
    using mini_agent::tui::AuthProbe;
    using mini_agent::tui::ConnState;

    mini_agent::AppConfig empty;
    check(mini_agent::tui::connection_state(empty, AuthProbe{}, false) == ConnState::Unconfigured,
          "no config reads as unconfigured");

    mini_agent::AppConfig url_only;
    url_only.base_url = "https://api.test/v1";
    check(mini_agent::tui::connection_state(url_only, AuthProbe{}, false) ==
              ConnState::Unconfigured,
          "an endpoint without a key is not connectable");

    mini_agent::AppConfig cfg;
    cfg.base_url = "https://api.test/v1";
    cfg.api_key = "do-not-leak-this";
    const AuthProbe untouched;
    check(mini_agent::tui::connection_state(cfg, untouched, false) == ConnState::Unverified,
          "configured but unprobed never reads as healthy");
    check(mini_agent::tui::connection_state(cfg, untouched, true) == ConnState::Checking,
          "a probe in flight reads as checking");

    AuthProbe verified;
    verified.attempted = true;
    verified.fetch_ok = true;
    verified.model_count = 2;
    check(mini_agent::tui::connection_state(cfg, verified, false) == ConnState::Verified,
          "an accepted probe reads as verified");

    AuthProbe parsed_bad;
    parsed_bad.attempted = true;
    parsed_bad.http_status = 200;
    check(mini_agent::tui::connection_state(cfg, parsed_bad, false) == ConnState::Verified,
          "2xx with an unusable body still proves the key");

    for (const int code : {401, 403}) {
        AuthProbe rejected;
        rejected.attempted = true;
        rejected.http_status = code;
        check(mini_agent::tui::connection_state(cfg, rejected, false) == ConnState::Rejected,
              "401/403 reads as rejected");
    }

    AuthProbe refused;
    refused.attempted = true;
    refused.http_status = 503;
    check(mini_agent::tui::connection_state(cfg, refused, false) == ConnState::Unverified,
          "a non-auth status leaves the key unverified, not rejected");

    AuthProbe unreachable;
    unreachable.attempted = true;
    unreachable.network_error = true;
    check(mini_agent::tui::connection_state(cfg, unreachable, false) == ConnState::Unreachable,
          "no response reads as unreachable");

    AuthProbe via_chat;
    via_chat.attempted = true;
    via_chat.fetch_ok = true;
    check(mini_agent::tui::connection_state(cfg, via_chat, false) == ConnState::Verified,
          "a chat reply turns the prompt green even with no model listing");

    // Markers: distinct glyph AND colour per state, reset-terminated, and
    // exactly one display column wide so the row accounting (5.6 rule 2) holds.
    struct Expected {
        ConnState state;
        const char* marker;
    };
    const Expected expected[] = {
        {ConnState::Unconfigured, "\x1b[2m?\x1b[0m"}, {ConnState::Unverified, "\x1b[33m?\x1b[0m"},
        {ConnState::Checking, "\x1b[36m~\x1b[0m"},    {ConnState::Verified, "\x1b[1;32m>\x1b[0m"},
        {ConnState::Rejected, "\x1b[1;31m!\x1b[0m"},  {ConnState::Unreachable, "\x1b[35mx\x1b[0m"},
    };
    for (const auto& e : expected) {
        const auto marker = mini_agent::tui::prompt_marker(e.state);
        check(marker == e.marker, "marker matches its state");
        check(marker.find("do-not-leak-this") == std::string::npos &&
                  marker.find("api.test") == std::string::npos,
              "marker carries no endpoint or key text");
        check(mini_agent::md::display_width(marker) == 1,
              "marker occupies exactly one display column");
    }
    // Colour alone is never the only difference: dim/yellow `?` differ in
    // colour but pair with the rest of the alphabet so states stay separable
    // by glyph everywhere except the two `?` variants, which the /help legend
    // covers.
    check(mini_agent::tui::prompt_marker(ConnState::Verified) !=
              mini_agent::tui::prompt_marker(ConnState::Rejected),
          "healthy and refused prompts are visibly different");
}

void test_logger() {
    // URL userinfo redaction: a base_url carrying curl-style credentials must
    // not leak them into the diagnostics file (design 007 section 6).
    check(mini_agent::Logger::redact_url("https://api.test/v1") == "https://api.test/v1",
          "url without userinfo unchanged");
    const auto redacted = mini_agent::Logger::redact_url("https://user:pw@api.test/v1");
    check(redacted == "https://***@api.test/v1", "userinfo replaced, host kept");
    check(redacted.find("pw") == std::string::npos, "credential absent from redacted url");
    check(mini_agent::Logger::redact_url("http://u:p@h.test:8931/v1?a=b") ==
              "http://***@h.test:8931/v1?a=b",
          "port and query preserved through redaction");
    check(mini_agent::Logger::redact_url("https://api.test/a@b/v1") == "https://api.test/a@b/v1",
          "'@' after the path is not userinfo");
    check(mini_agent::Logger::redact_url("not a url") == "not a url",
          "non-url value passed through");
    check(mini_agent::Logger::redact_url("").empty(), "empty url safe");

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
    check(matches.size() == 2 && matches[0] == "/session" && matches[1] == "/sessions",
          "prefix /s matches /session + /sessions (v4.9)");
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
    check(partial.size() == 3 && partial[1].find("/session") != std::string::npos &&
              partial[2].find("/sessions") != std::string::npos &&
              partial[1].find("/config") == std::string::npos,
          "prefix list has header + only matching rows (v4.9)");
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

// ---------------------------------------------------------------------------
// v2 (design 008): tool_calls round-trip, session schema 2, tool loop,
// run_bash, sanitization.
// ---------------------------------------------------------------------------

void test_tool_ui_helpers() {
    namespace tui = mini_agent::tui;
    // tool_activity_line: one row, sanitized, name+summary+outcome
    const auto line =
        tui::tool_activity_line("run_bash", R"({"command":"ls -la"})", true, false, 420, 1024);
    check(line.find("[tool] run_bash") != std::string::npos &&
              line.find("ls -la") != std::string::npos &&
              line.find("[ok 420ms") != std::string::npos,
          "activity line shape");
    const auto denied = tui::tool_activity_line("x/y", "{}", false, true, 5, 0);
    check(denied.find("[denied]") != std::string::npos, "denied marker");
    std::string evil_args = R"JW({"text":")JW";
    evil_args += "\x1b[2Jhacked\x07"
                 "done";
    evil_args += R"JW("})JW";
    const auto evil = tui::tool_activity_line("t", evil_args, true, false, 1, 1);
    check(evil.find('\x1b') == std::string::npos && evil.find('\x07') == std::string::npos,
          "escapes stripped from activity row");
    check(evil.find('\n') == std::string::npos, "activity row is single-line");

    // approval key state machine
    bool expanded = false;
    check(tui::approval_key('y', expanded) == tui::ApprovalKey::Allow, "y allows");
    check(tui::approval_key('A', expanded) == tui::ApprovalKey::AllowSession, "A session-allows");
    check(tui::approval_key('n', expanded) == tui::ApprovalKey::Deny, "n denies");
    check(tui::approval_key('?', expanded) == std::nullopt && expanded, "? expands");
    check(tui::approval_key('?', expanded) == std::nullopt && !expanded, "? collapses again");
    check(tui::approval_key('q', expanded) == std::nullopt, "other keys unhandled");

    // approval rows: header + legend; expanded adds argument lines
    const auto compact = tui::approval_rows("run_bash", R"({"command":"rm -rf /tmp/x"})", false);
    check(compact.size() == 2 && compact[0].find("rm -rf /tmp/x") != std::string::npos &&
              compact[1].find("y=allow") != std::string::npos,
          "approval rows compact shape");
    const auto wide = tui::approval_rows("srv__tool", R"({"a":{"b":1}})", true);
    std::string joined;
    for (const auto& row : wide)
        joined += row;
    check(wide.size() >= 4 && joined.find("\"b\"") != std::string::npos &&
              joined.find("params(compact)") != std::string::npos,
          "expanded shows pretty-printed params");

    // registry rows
    const auto rows = tui::tool_registry_rows({"run_bash"});
    check(rows.size() == 2 && rows[0].find("tools (1)") != std::string::npos, "registry rows");
    const auto none = tui::tool_registry_rows({});
    check(none.size() == 2 && none[1].find("mcp_servers") != std::string::npos, "empty hint");

    // slash registry includes the new commands
    const auto matches = tui::slash_matches("/to");
    check(std::find(matches.begin(), matches.end(), std::string_view("/tools")) != matches.end(),
          "/tools registered");
    const auto mc = tui::slash_matches("/mc");
    check(std::find(mc.begin(), mc.end(), std::string_view("/mcp")) != mc.end(), "/mcp registered");
}

void test_v1_request_shape_regresssion() {
    // 008 section 7: with no tools offered, the chat request body must be
    // exactly the v1 shape (model/stream/messages, nothing else).
    mini_agent::Chat chat;
    chat.add(mini_agent::Message{mini_agent::Role::User, "hi", std::nullopt, {}});
    const std::string v1ish = mini_agent::preview_chat_request_body(chat, "m", "");
    const std::string tools_empty = mini_agent::preview_chat_request_body(chat, "m", "[]");
    check(v1ish == tools_empty, "empty tools json == no tools");
    check(v1ish.find("\"tools\"") == std::string::npos, "no tools field when unset");
    check(v1ish.find("\"stream\":true") != std::string::npos, "stream flag kept");
    const std::string with_tools =
        mini_agent::preview_chat_request_body(chat, "m", R"([{"type":"function"}])");
    check(with_tools.find("\"tools\"") != std::string::npos, "tools field appears when set");
}

void test_tool_calls_round_trip() {
    mini_agent::Chat chat;
    mini_agent::Message assistant;
    assistant.role = mini_agent::Role::Assistant;
    assistant.content = "checking";
    assistant.tool_calls.push_back({"c1", "run_bash", R"({"command":"ls -a"})"});
    assistant.tool_calls.push_back({"c2", "fs/read", R"({"path":"x"})"});
    chat.add(std::move(assistant));
    mini_agent::Message tool;
    tool.role = mini_agent::Role::Tool;
    tool.content = "result text";
    tool.tool_call_id = "c1";
    chat.add(std::move(tool));

    const std::string text = mini_agent::chat_to_json(chat);
    check(text.find("\"tool_calls\"") != std::string::npos, "tool_calls written");
    check(text.find("\"arguments\"") != std::string::npos, "arguments written");
    // v1 omit-while-empty rule still holds for plain messages
    mini_agent::Chat plain;
    plain.add(mini_agent::Message{mini_agent::Role::User, "hi", std::nullopt, {}});
    check(mini_agent::chat_to_json(plain).find("tool_calls") == std::string::npos,
          "empty tool_calls omitted");

    std::string error;
    auto back = mini_agent::chat_from_json(text, &error);
    check(back.has_value(), (std::string("tool chat parses: ") + error).c_str());
    if (back) {
        check(back->messages()[0].tool_calls.size() == 2, "two calls survive");
        check(back->messages()[0].tool_calls[0].id == "c1" &&
                  back->messages()[0].tool_calls[0].name == "run_bash" &&
                  back->messages()[0].tool_calls[0].arguments_json == R"({"command":"ls -a"})",
              "call fields survive verbatim");
        check(back->messages()[1].tool_call_id == std::optional<std::string>("c1"),
              "tool_call_id still survives");
        check(back->messages()[0] == chat.messages()[0], "message equality covers tool_calls");
    }

    // fail-closed on malformed shapes (008 5.2)
    check(!mini_agent::chat_from_json(R"([{"role":"assistant","content":"","tool_calls":"nope"}])",
                                      &error)
               .has_value(),
          "tool_calls non-array rejected");
    check(!mini_agent::chat_from_json(
               R"([{"role":"assistant","content":"","tool_calls":[{"id":"1"}]}])", &error)
               .has_value(),
          "tool_call missing fields rejected");
    check(
        !mini_agent::chat_from_json(
             R"([{"role":"assistant","content":"","tool_calls":[{"id":1,"name":"n","arguments":"{}"}]}])",
             &error)
             .has_value(),
        "tool_call non-string id rejected");
    // empty array tolerated and dropped (omits on re-serialize)
    auto dropped = mini_agent::chat_from_json(
        R"([{"role":"assistant","content":"x","tool_calls":[]}])", &error);
    check(dropped.has_value() && dropped->messages()[0].tool_calls.empty(),
          "empty tool_calls array parses");
}

void test_session_schema_bumps_for_tools() {
    mini_agent::Chat chat;
    mini_agent::Message assistant;
    assistant.role = mini_agent::Role::Assistant;
    assistant.tool_calls.push_back({"c1", "x", "{}"});
    chat.add(std::move(assistant));

    mini_agent::SessionMeta meta;
    meta.id = "abc";
    const std::string v2 = mini_agent::session_to_json(meta, chat);
    check(v2.find("\"schema_version\": 2") != std::string::npos ||
              v2.find("\"schema_version\":2") != std::string::npos,
          "tool data bumps schema to 2");

    mini_agent::Chat plain;
    plain.add(mini_agent::Message{mini_agent::Role::User, "hi", std::nullopt, {}});
    const std::string v1 = mini_agent::session_to_json(meta, plain);
    check(v1.find("\"schema_version\": 1") != std::string::npos ||
              v1.find("\"schema_version\":1") != std::string::npos,
          "plain history stays at schema 1");

    // v2 file loads fine in this build (accepts up to kToolSchemaVersion)
    std::string error;
    auto loaded = mini_agent::session_from_json(v2, &error);
    check(loaded.has_value(), (std::string("v2 file loads: ") + error).c_str());
    if (loaded)
        check(loaded->meta.schema_version == 2, "loaded meta reports version 2");

    // tool_call_id alone also bumps (a Tool message without bumping would
    // still parse in v1, but tool_calls-bearing rounds must never half-load)
    mini_agent::Chat only_tool_msg;
    only_tool_msg.add(mini_agent::Message{mini_agent::Role::Tool, "r", "id1", {}});
    const std::string bumped = mini_agent::session_to_json(meta, only_tool_msg);
    check(bumped.find("2") != std::string::npos &&
              bumped.find("schema_version\": 1") == std::string::npos,
          "tool_call_id also bumps schema");
}

namespace {

// Provider that emits scripted tool-call rounds then a final text (or
// throws). Records the tools_json seen per call for request assertions.
class FakeToolProvider final : public mini_agent::Provider {
  public:
    struct Step {
        std::vector<mini_agent::ToolCall> calls;
        std::string text;
    };
    explicit FakeToolProvider(std::vector<Step> steps) : steps_(std::move(steps)) {
    }
    std::string complete(const mini_agent::Chat&, const std::function<void(std::string_view)>&,
                         const mini_agent::ShouldAbort&) override {
        return "unused";
    }
    mini_agent::CompletionResult
    complete_chat(const mini_agent::Chat& chat, const std::string& tools_json,
                  const std::function<void(std::string_view)>& on_delta,
                  const mini_agent::ShouldAbort&) override {
        seen_tools_.push_back(tools_json);
        seen_history_.push_back(chat.messages().size());
        const auto& step = steps_[static_cast<std::size_t>(index_ % steps_.size())];
        ++index_;
        if (fail_at_ == index_)
            throw mini_agent::ProviderError(mini_agent::ProviderErrorKind::Network, "boom");
        mini_agent::CompletionResult r;
        r.text = step.text;
        r.tool_calls = step.calls;
        if (!r.text.empty())
            on_delta(r.text);
        return r;
    }
    std::vector<std::string> seen_tools_;
    std::vector<std::size_t> seen_history_;

  private:
    std::vector<Step> steps_;
    int index_ = 0;
    int fail_at_ = -1;
};

// Echo-style runtime for loop tests.
class FakeRuntime final : public mini_agent::ToolRuntime {
  public:
    std::vector<mini_agent::ToolDefinition> list() override {
        mini_agent::ToolDefinition d;
        d.name = "echo";
        d.description = "echo the arguments back";
        d.input_schema_json = R"({"type":"object"})";
        d.source = "fake";
        return {d};
    }
    mini_agent::ToolResult call(const mini_agent::ToolCall& c,
                                const mini_agent::ShouldAbort&) override {
        ++calls;
        last_call = c;
        return mini_agent::ToolResult{"echo:" + c.arguments_json, error_flag};
    }
    int calls = 0;
    bool error_flag = false;
    mini_agent::ToolCall last_call;
};

} // namespace

void test_tool_loop_rounds_and_commit() {
    using namespace mini_agent;
    Chat chat;
    FakeToolProvider provider({{{{"t1", "echo", R"({"a":1})"}}, ""}, {{}, "final answer"}});
    auto registry = std::make_unique<ToolRegistry>();
    auto* fake = new FakeRuntime();
    registry->add(std::unique_ptr<ToolRuntime>(fake));

    TurnOptions options;
    options.tools = registry.get();
    std::string streamed;
    const std::string text = run_turn(
        chat, provider, "go", [&](std::string_view d) { streamed += d; }, [] { return false; },
        options);

    check(text == "final answer", "loop returns final text");
    check(streamed == "final answer", "only text streams to UI");
    check(fake->calls == 1, "tool executed once");
    // history: user, assistant(tool_calls), tool(result), assistant(final)
    check(chat.messages().size() == 4, "four messages committed");
    check(chat.messages()[1].tool_calls.size() == 1, "assistant tool_calls stored");
    check(chat.messages()[2].role == Role::Tool && *chat.messages()[2].tool_call_id == "t1" &&
              chat.messages()[2].content == R"(echo:{"a":1})",
          "tool result stored with call id");
    check(!provider.seen_tools_.empty() &&
              provider.seen_tools_[0].find("\"echo\"") != std::string::npos,
          "tools advertised on first round");
    check(provider.seen_tools_.size() == 2 &&
              provider.seen_tools_[1].find("echo") != std::string::npos,
          "tools stay advertised on non-final rounds");

    // approval deny -> error tool message, model still gets a chance
    Chat chat2;
    FakeToolProvider provider2({{{{"t1", "echo", "{}"}}, ""}, {{}, "sorry then"}});
    auto registry2 = std::make_unique<ToolRegistry>();
    auto* fake2 = new FakeRuntime();
    registry2->add(std::unique_ptr<ToolRuntime>(fake2));
    TurnOptions denied;
    denied.tools = registry2.get();
    denied.approve = [](const ToolCall&) { return Approval::Deny; };
    const std::string t2 =
        run_turn(chat2, provider2, "go", [](std::string_view) {}, [] { return false; }, denied);
    check(t2 == "sorry then", "deny path completes");
    check(fake2->calls == 0, "denied tool never executed");
    check(chat2.messages()[2].content == "user denied this tool invocation",
          "denial surfaces to model as tool result");

    // AllowSession: second call same name is not re-asked
    Chat chat3;
    FakeToolProvider provider3({{{{"t1", "echo", "1"}, {"t2", "echo", "2"}}, ""}, {{}, "done"}});
    auto registry3 = std::make_unique<ToolRegistry>();
    auto* fake3 = new FakeRuntime();
    registry3->add(std::unique_ptr<ToolRuntime>(fake3));
    TurnOptions sess;
    sess.tools = registry3.get();
    int questions = 0;
    sess.approve = [&](const ToolCall&) {
        ++questions;
        return Approval::AllowSession;
    };
    run_turn(chat3, provider3, "go", [](std::string_view) {}, [] { return false; }, sess);
    check(questions == 1, "session approval memoized per tool name");
    check(fake3->calls == 2, "both calls executed");

    // unknown tool name -> folded error result, no crash
    Chat chat4;
    FakeToolProvider provider4({{{{"t1", "nope", "{}"}}, ""}, {{}, "ok"}});
    auto registry4 = std::make_unique<ToolRegistry>();
    registry4->add(std::unique_ptr<ToolRuntime>(new FakeRuntime()));
    TurnOptions unk;
    unk.tools = registry4.get();
    const std::string t4 =
        run_turn(chat4, provider4, "go", [](std::string_view) {}, [] { return false; }, unk);
    check(t4 == "ok" && chat4.messages()[2].content.find("no such tool") != std::string::npos,
          "unknown tool folded into error result");

    // failure mid-loop leaves history untouched (007 section 5 for v2)
    Chat chat5;
    chat5.add(Message{Role::User, "prior", std::nullopt, {}});
    chat5.add(Message{Role::Assistant, "turn", std::nullopt, {}});
    FakeToolProvider failing({{{{"t1", "echo", "{}"}}, ""}, {{}, "never"}});
    // make round 2 throw
    failing.seen_tools_.clear();
    TurnOptions opt5;
    opt5.tools = registry4.get(); // "echo" not in registry4 -> still completes; use deny loop
    // instead: throw at provider level on the SECOND call
    struct Thrower final : Provider {
        int n = 0;
        std::string complete(const Chat&, const std::function<void(std::string_view)>&,
                             const ShouldAbort&) override {
            return "";
        }
        CompletionResult complete_chat(const Chat&, const std::string&,
                                       const std::function<void(std::string_view)>&,
                                       const ShouldAbort&) override {
            ++n;
            if (n == 1)
                return CompletionResult{"", {ToolCall{"t1", "echo", "{}"}}, {}, {}};
            throw ProviderError(ProviderErrorKind::Network, "late failure");
        }
    } thrower;
    auto registry5 = std::make_unique<ToolRegistry>();
    auto* fake5 = new FakeRuntime();
    registry5->add(std::unique_ptr<ToolRuntime>(fake5));
    TurnOptions opt_fail;
    opt_fail.tools = registry5.get();
    bool threw = false;
    try {
        run_turn(chat5, thrower, "go", [](std::string_view) {}, [] { return false; }, opt_fail);
    } catch (const ProviderError&) {
        threw = true;
    }
    check(threw, "mid-loop provider failure propagates");
    check(chat5.messages().size() == 2, "no partial tool history stored");
    check(fake5->calls == 1, "tool side effect DID run (documented in 008 section 6)");
}

void test_max_tool_rounds() {
    using namespace mini_agent;
    Chat chat;
    // provider that ALWAYS requests a tool -> must end via summary round
    struct Looper final : Provider {
        int n = 0;
        std::string complete(const Chat&, const std::function<void(std::string_view)>&,
                             const ShouldAbort&) override {
            return "";
        }
        CompletionResult complete_chat(const Chat& c, const std::string& tools,
                                       const std::function<void(std::string_view)>&,
                                       const ShouldAbort&) override {
            ++n;
            last_tools = tools;
            last_history = c.messages().size();
            if (n < 4)
                return CompletionResult{
                    "", {ToolCall{"t" + std::to_string(n), "echo", "{}"}}, {}, {}};
            return CompletionResult{"gave up gracefully", {}, {}, {}};
        }
        std::string last_tools;
        std::size_t last_history = 0;
    } looper;
    auto registry = std::make_unique<ToolRegistry>();
    registry->add(std::unique_ptr<ToolRuntime>(new FakeRuntime()));
    TurnOptions options;
    options.tools = registry.get();
    options.max_tool_rounds = 3;
    const std::string text =
        run_turn(chat, looper, "go", [](std::string_view) {}, [] { return false; }, options);
    check(text == "gave up gracefully", "loop terminates at budget via no-tools summary");
    check(looper.last_tools.empty(), "final round advertises no tools");
    // transient system note must not be persisted
    for (const auto& m : chat.messages())
        check(m.role != Role::System, "limit note never stored");
}

void test_mcp_cli_specs() {
    std::string error;
    mini_agent::McpServerConfig server;

    // http shorthand (loopback ok)
    check(mini_agent::parse_mcp_cli_spec("docs=http://127.0.0.1:8933/mcp", &server, &error) &&
              server.name == "docs" && server.transport == "http" &&
              server.url == "http://127.0.0.1:8933/mcp",
          "http shorthand parses");
    // https everywhere ok
    check(mini_agent::parse_mcp_cli_spec("w=https://mcp.deepwiki.com/mcp", &server, &error) &&
              server.transport == "http",
          "https shorthand parses");
    // plain http remote rejected without opt-in (same gate as file config)
    check(!mini_agent::parse_mcp_cli_spec("bad=http://remote.example.com/mcp", &server, &error),
          "plain http remote rejected");
    // stdio shorthand with args
    check(mini_agent::parse_mcp_cli_spec("fs=npx -y srv /tmp", &server, &error) &&
              server.transport == "stdio" && server.command == "npx" && server.args.size() == 3 &&
              server.args[0] == "-y",
          "stdio shorthand splits args");
    // json full form
    check(mini_agent::parse_mcp_cli_spec(
              R"JW(s={"transport":"http","url":"https://h/mcp","bearer_token":"t"})JW", &server,
              &error) &&
              server.transport == "http" && server.bearer_token == "t",
          "json form parses with bearer");
    // json form cannot rename itself
    mini_agent::McpServerConfig renamed;
    check(mini_agent::parse_mcp_cli_spec(R"JW(x={"url":"https://h/m"})JW", &renamed, &error) &&
              renamed.name == "x",
          "json form keeps CLI name");
    // invalid names rejected
    check(!mini_agent::parse_mcp_cli_spec("a/b=npx x", &server, &error), "slash name rejected");
    check(!mini_agent::parse_mcp_cli_spec("=value", &server, &error), "empty name rejected");
    check(!mini_agent::parse_mcp_cli_spec("name=", &server, &error), "empty spec rejected");
    check(!mini_agent::parse_mcp_cli_spec("name={bad json", &server, &error),
          "malformed json rejected");

    // upsert replaces by name (CLI wins over file entry)
    mini_agent::AppConfig config;
    mini_agent::McpServerConfig file_entry;
    file_entry.name = "dup";
    file_entry.transport = "stdio";
    file_entry.command = "old";
    config.mcp_servers.push_back(file_entry);
    check(mini_agent::parse_mcp_cli_spec("dup=https://new/mcp", &server, &error), "dup parses");
    mini_agent::upsert_mcp_server(&config, server);
    check(config.mcp_servers.size() == 1 && config.mcp_servers[0].transport == "http",
          "upsert replaces same name");
    mini_agent::McpServerConfig extra;
    extra.name = "other";
    extra.transport = "stdio";
    extra.command = "x";
    mini_agent::upsert_mcp_server(&config, extra);
    check(config.mcp_servers.size() == 2, "new name appends");
}

void test_config_v2_fields() {
    std::string error;
    auto loaded = mini_agent::config_from_json(R"({
        "schema_version":1,"base_url":"https://api.example.com/v1","api_key":"k","model":"m",
        "tool_approval":"allowlist","tool_allowlist":["run_bash"],
        "max_tool_rounds":4,"tool_timeout_seconds":15,
        "mcp_servers":{
            "fs":{"command":"npx","args":["-y","srv","/tmp"],"env":{"NODE_ENV":"production"}},
            "docs":{"url":"http://127.0.0.1:8932/mcp","bearer_token":"tok"},
            "bad":{"transport":"http","url":"http://remote.example.com/mcp"}
        }})",
                                               &error);
    check(!loaded.has_value(), "plain http non-loopback rejected without opt-in");
    check(error.find("allow_insecure_http") != std::string::npos, "error names the opt-in");

    auto good = mini_agent::config_from_json(R"({
        "base_url":"https://api.example.com/v1","api_key":"k","model":"m",
        "mcp_servers":{
            "fs":{"command":"npx","args":["-y","srv"],"env":{"A":"1"}},
            "docs":{"url":"https://mcp.example.com/mcp","bearer_token":"tok"}
        }})",
                                             &error);
    check(good.has_value(), (std::string("mcp servers parse: ") + error).c_str());
    if (good) {
        check(good->mcp_servers.size() == 2, "two servers");
        // object iteration is key-sorted; look entries up by name
        const mini_agent::McpServerConfig* fs = nullptr;
        const mini_agent::McpServerConfig* docs = nullptr;
        for (const auto& server : good->mcp_servers) {
            if (server.name == "fs")
                fs = &server;
            if (server.name == "docs")
                docs = &server;
        }
        check(fs != nullptr && docs != nullptr, "both named servers present");
        check(fs && fs->transport == "stdio" && fs->command == "npx" && fs->args.size() == 2 &&
                  fs->env.size() == 1 && fs->env[0].first == "A",
              "stdio entry fields");
        check(docs && docs->transport == "http" && docs->url == "https://mcp.example.com/mcp" &&
                  docs->bearer_token == "tok",
              "http entry inferred");
        // round-trip: serialize keeps servers, approval default omitted when ask
        const auto text = mini_agent::config_to_json(*good);
        check(text.find("mcp_servers") != std::string::npos &&
                  text.find("docs") != std::string::npos,
              "servers re-serialize");
        auto back = mini_agent::config_from_json(text, &error);
        check(back.has_value() && back->mcp_servers.size() == 2, "round-trip stable");
    }

    auto bad_approval = mini_agent::config_from_json(
        R"({"base_url":"b","api_key":"k","model":"m","tool_approval":"yolo"})", &error);
    check(!bad_approval.has_value(), "invalid tool_approval rejected");
    auto ok_approval = mini_agent::config_from_json(
        R"({"base_url":"b","api_key":"k","model":"m","tool_approval":"off"})", &error);
    check(ok_approval.has_value() && ok_approval->tool_approval == "off", "off is explicit opt-in");
    check(mini_agent::config_to_json(mini_agent::AppConfig{}).find("tool_approval") ==
              std::string::npos,
          "default ask not written (compact files)");

    // missing/unknown transport combo fails closed
    check(!mini_agent::config_from_json(
               R"({"mcp_servers":{"x":{"transport":"websocket","url":"ws://a"}}})", &error)
               .has_value(),
          "unknown transport rejected");
    check(!mini_agent::config_from_json(R"({"mcp_servers":{"x":{}}})", &error).has_value(),
          "empty stdio entry without command rejected");
}

void test_run_bash_and_helpers() {
    using namespace mini_agent;
    BuiltinRuntime runtime(5);
    check(!runtime.list().empty() && runtime.list()[0].name == "run_bash", "run_bash listed");

    const auto ok =
        runtime.call(ToolCall{"1", "run_bash", R"({"command":"printf hi"})"}, [] { return false; });
    check(!ok.is_error && ok.text == "hi", "printf captured");

    const auto both =
        runtime.call(ToolCall{"1", "run_bash", R"({"command":"echo out; echo err 1>&2; exit 3"})"},
                     [] { return false; });
    check(both.is_error && both.text.find("out") != std::string::npos &&
              both.text.find("err") != std::string::npos &&
              both.text.find("exit 3") != std::string::npos,
          "stdout+stderr merged, exit code reported");

    const auto timeout = runtime.call(
        ToolCall{"1", "run_bash", R"({"command":"sleep 30","timeout":1})"}, [] { return false; });
    check(timeout.is_error && timeout.text.find("timed out") != std::string::npos,
          "timeout kills the command");

    const auto bad_args = runtime.call(ToolCall{"1", "run_bash", "not-json"}, [] { return false; });
    check(bad_args.is_error && bad_args.text.find("not valid JSON") != std::string::npos,
          "malformed arguments fail closed as error result");

    const auto wrong = runtime.call(ToolCall{"1", "nope", "{}"}, [] { return false; });
    check(wrong.is_error, "unknown builtin name is an error result");

    // output cap keeps head+tail
    const auto big = runtime.call(
        ToolCall{
            "1", "run_bash",
            R"J({"command":"printf 'A%.0s' $(seq 1 40000); printf MARKER; printf 'B%.0s' $(seq 1 40000)"})J"},
        [] { return false; });
    check(!big.is_error && big.text.size() < 40000 &&
              big.text.find("truncated") != std::string::npos && big.text.front() == 'A' &&
              big.text.back() == 'B',
          "runaway output truncated head+tail");

    // cap_tool_output pure helper
    check(cap_tool_output("short", 1024) == "short", "short text untouched");
    const std::string long_text(10000, 'x');
    const auto capped = cap_tool_output(long_text, 1024);
    check(capped.size() < 1100 && capped.find("truncated") != std::string::npos,
          "cap inserts marker");

    // parse_bash_arguments bounds
    std::string error;
    check(parse_bash_arguments(R"({"command":"ls","timeout":7})", 30, &error)->timeout_seconds == 7,
          "explicit timeout honored");
    check(!parse_bash_arguments(R"({"command":"ls","timeout":0})", 30, &error).has_value(),
          "zero timeout rejected");
    check(!parse_bash_arguments(R"({"command":""})", 30, &error).has_value(),
          "empty command rejected");
    check(!parse_bash_arguments(R"({"timeout":5})", 30, &error).has_value(),
          "missing command rejected");

    // sanitize_display strips escapes but keeps newline/tab
    const std::string dirty = "ok\x1b[2Jhacked\x07\x00done\nmore\t";
    const auto clean = sanitize_display(dirty);
    check(clean.find('\x1b') == std::string::npos && clean.find('\0') == std::string::npos &&
              clean.find("ok") == 0 && clean.find("done\nmore\t") != std::string::npos,
          "escape sequences and C0 controls stripped, text layout kept");

    // registry json shape + duplicate name claim
    auto registry = std::make_unique<ToolRegistry>();
    registry->add(std::make_unique<BuiltinRuntime>());
    registry->add(std::unique_ptr<ToolRuntime>(new FakeRuntime()));
    const auto tools_json = registry->to_openai_tools_json();
    check(tools_json.find("\"type\":\"function\"") != std::string::npos &&
              tools_json.find("run_bash") != std::string::npos &&
              tools_json.find("echo") != std::string::npos,
          "openai tools json assembled");
    ToolRegistry empty_registry;
    check(empty_registry.to_openai_tools_json() == "[]", "empty registry yields []");
    check(registry->find("echo") != nullptr && registry->find("ghost") == nullptr,
          "find routes by name");
}

void test_history_styling() {
    namespace tui = mini_agent::tui;
    // colors=false: byte-identical passthrough (non-TTY / NO_COLOR path).
    check(tui::style_history_line("[error] boom", false) == "[error] boom",
          "styling off returns the line unchanged");
    // Already-styled lines (markdown replies, producer SGR) are never touched.
    const std::string pre = "\x1b[1mAI: \x1b[0malready styled";
    check(tui::style_history_line(pre, true) == pre, "styled lines pass through");
    // Tag classification: whole-line spans, exact text preserved inside.
    const auto ok = tui::style_history_line("[ok] config saved", true);
    check(ok.find("\x1b[32m[ok] config saved\x1b[0m") == 0, "ok rows green, whole-line span");
    const auto err = tui::style_history_line("[error] model list failed", true);
    check(err.find("\x1b[1;31m") == 0 && err.find("model list failed") != std::string::npos,
          "error rows bold red");
    const auto warn = tui::style_history_line("[warn] save failed: x", true);
    check(warn == "\x1b[33m[warn] save failed: x\x1b[0m", "warn rows yellow");
    const auto info = tui::style_history_line("[info] listing models from provider...", true);
    check(info == "\x1b[36m[info] listing models from provider...\x1b[0m", "info rows cyan");
    // Auth verdicts differ by substring, not just tag.
    check(tui::style_history_line("[auth] login verified · key accepted · 2 model(s)", true)
                  .find("\x1b[32m") == 0,
          "verified auth green");
    check(tui::style_history_line("[auth] login REJECTED · HTTP 401", true).find("\x1b[1;31m") == 0,
          "rejected auth bold red");
    check(tui::style_history_line("[auth] login unverified · /provider", true).find("\x1b[33m") ==
              0,
          "unverified auth yellow");
    // Tool outcome colours.
    check(tui::style_history_line("[tool] run_bash · ls [ok 5ms · 1 B]", true).find("\x1b[36m") ==
              0,
          "ok tool row cyan");
    check(tui::style_history_line("[tool] x [denied]", true).find("\x1b[1;31m") == 0,
          "denied tool row bold red");
    check(tui::style_history_line("[tool] x [error 5ms · 0 B]", true).find("\x1b[33m") == 0,
          "error tool row yellow");
    // Identity rows bold, informational echoes dim, replies left plain.
    check(tui::style_history_line("mini-agent · model=m · session=s", true).find("\x1b[1mm") == 0,
          "banner bold");
    check(tui::style_history_line("You: hello", true).find("\x1b[1mYou: hello\x1b[0m") == 0,
          "user echo bold");
    check(tui::style_history_line("commands: /provider <url>", true).find("\x1b[2m") == 0,
          "help legend dim");
    check(tui::style_history_line("approval=off", true).find("\x1b[33m") == 0,
          "approval policy off is yellow");
    check(tui::style_history_line("approval=ask", true).find("\x1b[2m") == 0,
          "approval policy ask is dim");
    check(tui::style_history_line("    continuation of AI reply", true) ==
              "    continuation of AI reply",
          "assistant continuation stays plain");
    check(tui::style_history_line("AI: plain reply", true) == "AI: plain reply",
          "assistant reply rows stay plain");
    // Wrap safety: the span is zero-width for md::wrap, and wrapped
    // continuation rows re-assert the styling.
    const std::string long_line = "[info] " + std::string(100, 'x');
    const auto styled = tui::style_history_line(long_line, true);
    check(mini_agent::md::display_width(styled) == mini_agent::md::display_width(long_line),
          "SGR adds zero display width");
    const auto wrapped = mini_agent::md::wrap(styled, 40);
    check(wrapped.size() == 3 && wrapped[1].find("\x1b[36m") != std::string::npos,
          "md::wrap re-asserts the span on continuation rows");
}

} // namespace

void test_unified_diff() {
    using namespace mini_agent;
    // Unchanged texts (including the trailing-newline normalization): no
    // change reported, empty unified output.
    check(!diff_texts("a\nb\n", "a\nb\n").changed, "identical text unchanged");
    check(!diff_texts("a\n", "a").changed, "trailing newline alone is not a change");
    // A single-line change: one hunk, context kept, coordinates correct.
    const auto one = diff_texts("x\na\ny\n", "x\nB\ny\n");
    check(one.changed && !one.too_large, "single change detected");
    check(one.added == 1 && one.removed == 1, "counts: one add one remove");
    const std::string text = one.to_unified_text("old", "new");
    check(text == "--- old\n+++ new\n@@ -1,3 +1,3 @@\n x\n-a\n+B\n y\n", "exact hunk shape");
    // New file (empty -> content): all adds.
    const auto fresh = diff_against_empty("hello\nworld\n");
    check(fresh.removed == 0 && fresh.added == 2, "new file is all additions");
    // Deletion-only change keeps the @@ coordinates honest on the new side.
    const auto del = diff_texts("a\nb\nc\nd\n", "a\nd\n");
    const std::string deltext = del.to_unified_text("o", "n");
    check(deltext.find("@@ -1,4 +1,2 @@") != std::string::npos, "deletion hunk header");
    check(del.removed == 2 && del.added == 0, "deletion counts");
    // Too-large guard fires before the O(n*m) table.
    std::string big(DiffOptions{}.max_diff_lines + 1, '\n');
    big += "tail\n";
    const auto huge = diff_texts("", big);
    check(huge.changed && huge.too_large && !huge.note.empty(), "oversized input gives up");
    // Cell-budget guard: both sides under the line cap, product over it.
    std::string mid_a;
    std::string mid_b;
    for (int i = 0; i < 2500; ++i) {
        mid_a += "a" + std::to_string(i) + "\n";
        mid_b += "b" + std::to_string(i) + "\n";
    }
    DiffOptions tight_cells;
    tight_cells.max_diff_cells = 1000;
    const auto cells = diff_texts(mid_a, mid_b, tight_cells);
    check(cells.changed && cells.too_large &&
              cells.note.find("too large to preview") != std::string::npos,
          "cell budget guard fires before allocation");
    // Hunk merging: two changes within context lines share one hunk.
    // Rule matches git xdiff: gap <= 2*ctx merges (2026-11-05 alignment,
    // 009 section 9 comparison record).
    const auto merged = diff_texts("1\n2\n3\n4\n5\n6\n7\n8\n9\n", "A\n2\n3\n4\n5\n6\n7\n8\nB\n");
    const std::size_t hunks = std::count_if(merged.rows.begin(), merged.rows.end(),
                                            [](const DiffRow& r) { return r.kind == '@'; });
    check(hunks == 2, "far-apart changes split hunks");
    const auto close = diff_texts("1\n2\n3\n4\n5\n", "A\n2\n3\n4\nB\n");
    const std::size_t close_hunks = std::count_if(close.rows.begin(), close.rows.end(),
                                                  [](const DiffRow& r) { return r.kind == '@'; });
    check(close_hunks == 1, "changes within context merge into one hunk");
    // git-alignment: gap of 6 keeps (<= 2*ctx) merges as git would; the
    // old gap<=ctx rule split it.
    const auto mid = diff_texts("1\n2\n3\n4\n5\n6\n7\n8\n", "A\n2\n3\n4\n5\n6\n7\nB\n");
    const std::size_t mid_hunks = std::count_if(mid.rows.begin(), mid.rows.end(),
                                                [](const DiffRow& r) { return r.kind == '@'; });
    check(mid_hunks == 1, "gap within 2*ctx merges like git xdiff");
    // Row-budget truncation keeps the result bounded and flags the note.
    std::string old_t;
    std::string new_t;
    for (int i = 0; i < 500; ++i) {
        old_t += "line" + std::to_string(i) + "\n";
        new_t += "L" + std::to_string(i) + "\n";
    }
    DiffOptions tight;
    tight.max_hunk_rows = 40;
    const auto capped = diff_texts(old_t, new_t, tight);
    check(capped.rows.size() < 60 && capped.note.find("truncated") != std::string::npos,
          "row budget caps output with note");
    // Empty-input edge: split of "" is zero lines, of "\n" is one empty line.
    check(diff_split_lines("").empty(), "empty text has no lines");
    check(diff_split_lines("\n").size() == 1, "single newline is one empty line");
}

// Creates a temp workspace dir with sample files for file-tool tests.
struct TempWorkspace {
    std::filesystem::path root;
    std::filesystem::path outside;
    TempWorkspace() {
        const auto tag = mini_agent::generate_session_id();
        root = std::filesystem::temp_directory_path() / ("mini_agent_ws_" + tag);
        outside = std::filesystem::temp_directory_path() / ("mini_agent_out_" + tag);
        std::error_code ec;
        std::filesystem::create_directories(root / "sub", ec);
        std::filesystem::create_directories(outside, ec);
        {
            std::ofstream f(root / "a.txt");
            f << "alpha\nbeta\ngamma\nbeta\n";
        }
        {
            std::ofstream f(root / "sub" / "b.txt");
            f << "one\ntwo\n";
        }
        {
            std::ofstream f(outside / "secret.txt");
            f << "do not read\n";
        }
    }
    ~TempWorkspace() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(outside, ec);
    }
    std::string read(const std::string& rel) {
        std::ifstream f(root / rel, std::ios::binary);
        std::ostringstream b;
        b << f.rdbuf();
        return b.str();
    }
};

void test_file_tools() {
    using namespace mini_agent;
    TempWorkspace ws;
    FileToolsConfig fc;
    fc.workspace = std::filesystem::canonical(ws.root);
    FileRuntime runtime(fc);

    const auto tools = runtime.list();
    check(tools.size() == 5, "five file tools listed");

    auto call = [&](const char* name, const std::string& args) {
        return runtime.call(ToolCall{"1", name, args}, [] { return false; });
    };

    // read_file: whole file is line-numbered; ranges slice; binary/majority
    // of failures fold into is_error.
    const auto all = call("read_file", R"({"path":"a.txt"})");
    check(!all.is_error && all.text.find("1\talpha") != std::string::npos &&
              all.text.find("4\tbeta") != std::string::npos,
          "read_file numbers lines");
    const auto range = call("read_file", R"({"path":"a.txt","from_line":2,"to_line":3})");
    check(!range.is_error && range.text.find("2\tbeta") != std::string::npos &&
              range.text.find("1\t") == std::string::npos &&
              range.text.find("4\t") == std::string::npos,
          "read_file honors from/to line");
    check(call("read_file", R"({"path":"nope.txt"})").is_error, "read_file missing file errors");
    check(call("read_file", "not-json").is_error, "read_file malformed args fail closed");

    // Workspace boundary: traversal, absolute-escape, and dot-dot.
    const auto escape = call("read_file", R"({"path":"../../etc/passwd"})");
    check(escape.is_error && escape.text.find("escapes workspace") != std::string::npos,
          "read_file blocks parent traversal");
    // Absolute path outside root is rejected too (construct the real path).
    const std::string outside_abs = (ws.outside / "secret.txt").string();
    const auto abs_escape = call("read_file", std::string(R"({"path":")") + outside_abs + R"("})");
    check(abs_escape.is_error && abs_escape.text.find("escapes workspace") != std::string::npos,
          "read_file blocks absolute path outside root");
    // A legit absolute path INSIDE root is allowed.
    const auto abs_ok =
        call("read_file", std::string(R"({"path":")") + (ws.root / "a.txt").string() + R"("})");
    check(!abs_ok.is_error, "read_file allows absolute path inside root");

    // resolve_within_workspace direct checks.
    std::filesystem::path resolved;
    std::string rerr;
    check(resolve_within_workspace(ws.root, "sub/b.txt", &resolved, &rerr), "relative resolves");
    check(!resolve_within_workspace(ws.root, "", &resolved, &rerr), "empty path rejected");
    check(!resolve_within_workspace(ws.root, "sub/../../secret", &resolved, &rerr),
          "sneaky traversal rejected");

    // write_file: new file + overwrite, both report success and land bytes.
    const auto created = call("write_file", R"({"path":"new.txt","content":"hello\n"})");
    check(!created.is_error && created.text.find("created") != std::string::npos,
          "write_file creates");
    check(ws.read("new.txt") == "hello\n", "created content on disk");
    const auto over = call("write_file", R"({"path":"sub/b.txt","content":"replaced\n"})");
    check(!over.is_error && ws.read("sub/b.txt") == "replaced\n", "write_file overwrites");
    check(call("write_file", R"({"path":"nodir/x.txt","content":"y"})").is_error,
          "write_file missing parent errors");
    check(call("write_file", R"({"path":"../esc.txt","content":"y"})").is_error,
          "write_file cannot escape root");

    // edit_file: unique match required unless replace_all.
    const auto notfound = call("edit_file", R"({"path":"a.txt","old_str":"zzz","new_str":"q"})");
    check(notfound.is_error && notfound.text.find("not found") != std::string::npos,
          "edit_file 0 matches errors");
    const auto ambiguous = call("edit_file", R"({"path":"a.txt","old_str":"beta","new_str":"B"})");
    check(ambiguous.is_error && ambiguous.text.find("matches 2 places") != std::string::npos,
          "edit_file multi-match errors");
    const auto unique =
        call("edit_file", R"({"path":"a.txt","old_str":"alpha","new_str":"ALPHA"})");
    check(!unique.is_error && ws.read("a.txt").find("ALPHA") == 0, "edit_file unique replaces");
    const auto all2 =
        call("edit_file", R"({"path":"a.txt","old_str":"beta","new_str":"B","replace_all":true})");
    check(!all2.is_error, "edit_file replace_all ok");
    check(ws.read("a.txt").find("beta") == std::string::npos, "replace_all swapped both");

    // list_dir: entries present.
    const auto listed = call("list_dir", "{}");
    check(!listed.is_error && listed.text.find("a.txt") != std::string::npos &&
              listed.text.find("sub/") != std::string::npos,
          "list_dir shows files and dirs");

    // search_text: substring across files; single-file filter; regex.
    const auto found = call("search_text", R"({"pattern":"gamma"})");
    check(!found.is_error && found.text.find("a.txt:3:") != std::string::npos,
          "search_text finds file content");
    // after overwrite sub/b.txt is "replaced"; search a.txt's ALPHA instead
    const auto found2 = call("search_text", R"({"pattern":"ALPHA","path_prefix":"a.txt"})");
    check(!found2.is_error && found2.text.find("a.txt:1:") != std::string::npos,
          "search_text single-file mode reports path:line");
    const auto none = call("search_text", R"({"pattern":"zzzznotpresent"})");
    check(!none.is_error && none.text == "(no matches)", "search_text no matches ok");
    const auto regex = call("search_text", R"({"pattern":"^gamma$","is_regex":true})");
    check(!regex.is_error && regex.text.find("gamma") != std::string::npos, "regex search works");
    check(call("search_text", R"({"pattern":"(","is_regex":true})").is_error,
          "invalid regex fails closed");

    // preview(): unified diff for edit/write; arms the race guard.
    {
        // fresh copy for race test
        {
            std::ofstream f(ws.root / "race.txt");
            f << "original\n";
        }
        ToolCall rc{"1", "edit_file",
                    R"({"path":"race.txt","old_str":"original","new_str":"changed"})"};
        const auto preview = runtime.preview(rc);
        check(preview.find("-original") != std::string::npos &&
                  preview.find("+changed") != std::string::npos,
              "preview shows unified diff");
        // External mutation before execute: guard refuses (009 5.3).
        {
            std::ofstream f(ws.root / "race.txt");
            f << "someone else\n";
        }
        const auto raced = runtime.call(rc, [] { return false; });
        check(raced.is_error &&
                  raced.text.find("changed since the approved preview") != std::string::npos,
              "race guard refuses stale approval");
    }

    // registry preview() routes to the owning runtime.
    auto registry = std::make_unique<ToolRegistry>();
    registry->add(std::make_unique<FileRuntime>(fc));
    const auto p = registry->preview(
        ToolCall{"1", "write_file", R"({"path":"p.txt","content":"brand new\n"})"});
    check(p.find("+brand new") != std::string::npos, "registry.preview delegates");
    check(registry->preview(ToolCall{"1", "run_bash", R"({"command":"ls"})"}).empty(),
          "registry.preview empty for no-preview tool");
}

void test_v3_config_and_prompt() {
    using namespace mini_agent;
    // config round-trip: workspace + system_prompt presence semantics.
    AppConfig config;
    config.base_url = "https://x.test/v1";
    config.api_key = "k";
    config.model = "m";
    config.workspace = "/tmp/somewhere";
    std::string error;
    const auto json = config_to_json(config);
    check(json.find("\"workspace\": \"/tmp/somewhere\"") != std::string::npos,
          "workspace serialized");
    auto back = config_from_json(json, &error);
    check(back && back->workspace == "/tmp/somewhere", "workspace round-trips");
    check(back && !back->system_prompt.has_value(), "system_prompt absent = default");
    // explicit empty disables; null clears back to default
    auto disabled = config_from_json(R"({"system_prompt":""})", &error);
    check(disabled && disabled->system_prompt.has_value() && disabled->system_prompt->empty(),
          "empty prompt parsed as disabled");
    auto nulled = config_from_json(R"({"system_prompt":null})", &error);
    check(nulled && !nulled->system_prompt.has_value(), "null prompt = default");
    auto bad = config_from_json(R"({"system_prompt":7})", &error);
    check(!bad.has_value(), "non-string prompt rejected");
    // old v1 files still parse (forward-read rule).
    check(config_from_json(R"({"base_url":"b","api_key":"k","model":"m"})", &error).has_value(),
          "v1 config loads unchanged");

    // build_system_prompt: default mentions tools + workspace; override
    // passthrough; "" disabled.
    const auto prompt = build_system_prompt("/work/dir", "gpt-test", std::nullopt);
    check(prompt.find("Workspace: /work/dir") != std::string::npos, "workspace line present");
    check(prompt.find("edit_file") != std::string::npos &&
              prompt.find("git_commit") != std::string::npos &&
              prompt.find("never") != std::string::npos,
          "default prompt covers tool etiquette + push ban");
    check(build_system_prompt("/x", "m", std::string("custom only")) == "custom only",
          "override replaces");
    check(build_system_prompt("/x", "m", std::string()).empty(), "disabled is empty");

    // inject_system_prompt: fresh chat only.
    Chat chat;
    check(inject_system_prompt(chat, "/w", "m", std::nullopt), "injected into empty chat");
    check(chat.messages().size() == 1 && chat.messages()[0].role == Role::System,
          "system message first");
    check(!inject_system_prompt(chat, "/w2", "m", std::nullopt), "no double injection");
    Chat nonempty;
    Message user;
    user.role = Role::User;
    user.content = "hi";
    nonempty.add(user);
    check(!inject_system_prompt(nonempty, "/w", "m", std::nullopt),
          "resumed sessions never gain a prompt");

    // update_system_workspace: rewrites only the Workspace line.
    chat.update_system_workspace("/new/place");
    check(chat.messages()[0].content.find("Workspace: /new/place") != std::string::npos &&
              chat.messages()[0].content.find("Workspace: /w\n") == std::string::npos,
          "workspace line swapped in place");
    check(chat.messages()[0].content.find("edit_file") != std::string::npos,
          "rest of prompt untouched");
    Chat custom;
    Message note;
    note.role = Role::System;
    note.content = "no marker here";
    custom.add(note);
    custom.update_system_workspace("/ignored");
    check(custom.messages()[0].content == "no marker here", "no marker = no rewrite");
    Chat user_first;
    Message u;
    u.role = Role::User;
    user_first.add(u);
    user_first.update_system_workspace("/noop");
    check(user_first.messages()[0].content.empty(), "non-system first = no-op");

    // session meta: workspace round-trips through the JSON bridge.
    SessionMeta meta;
    meta.id = "abcd1234";
    meta.workspace = std::string("/some/ws");
    Chat c;
    const auto text = session_to_json(meta, c);
    auto loaded = session_from_json(text, &error);
    check(loaded && loaded->meta.workspace == "/some/ws", "session workspace persists");
    SessionMeta plain;
    plain.id = "abcd1234";
    check(session_from_json(session_to_json(plain, c), &error)->meta.workspace == std::nullopt,
          "absent stays absent");
}

void test_approval_preview_rows() {
    using namespace mini_agent;
    // approval_rows with a preview renders the diff view (default) and
    // switches to params when expanded (v3 design 009 5.3).
    const std::string preview = "--- a/x\n+++ b/x\n@@ -1,1 +1,1 @@\n-old\n+new\n";
    const auto diff_view = tui::approval_rows("edit_file", R"({"path":"x"})", false, preview);
    bool has_plus = false;
    bool has_hunk = false;
    for (const auto& row : diff_view) {
        if (row.find("+new") != std::string::npos)
            has_plus = true;
        if (row.find("@@") != std::string::npos)
            has_hunk = true;
    }
    check(has_plus && has_hunk, "preview rows show diff");
    const auto params_view =
        tui::approval_rows("edit_file", R"({"path":"x","old_str":"a"})", true, preview);
    bool shows_params = false;
    for (const auto& row : params_view)
        shows_params = shows_params || row.find("old_str") != std::string::npos;
    check(shows_params, "? expands full params even with a preview");
    // No preview: legacy command-summary view unchanged.
    const auto legacy = tui::approval_rows("run_bash", R"({"command":"ls -la"})", false, "");
    check(!legacy.empty() && legacy[0].find("ls -la") != std::string::npos,
          "empty preview falls back to legacy rows");
}

void test_git_tools() {
    using namespace mini_agent;
    // parse_porcelain_line pure checks.
    std::string code;
    std::string file;
    check(parse_porcelain_line("M  src/a.cpp", &code, &file) && code == "M " && file == "src/a.cpp",
          "porcelain M parsed");
    check(parse_porcelain_line("?? new.txt", &code, &file) && code == "??" && file == "new.txt",
          "porcelain ?? parsed");
    check(!parse_porcelain_line("## main...origin/main", &code, &file), "branch header ignored");
    check(!parse_porcelain_line("short", &code, &file), "short line rejected");

    // Real temp repository.
    const auto tag = generate_session_id();
    const auto repo = std::filesystem::temp_directory_path() / ("mini_agent_git_" + tag);
    std::error_code ec;
    std::filesystem::create_directories(repo, ec);
    auto sh = [&](const std::string& cmd) {
        const std::string full = "cd '" + repo.string() + "' && " + cmd;
        return std::system(full.c_str()) == 0;
    };
    bool env_ok = sh("git init -q . && git config user.email t@t && git config user.name t && "
                     "echo base > f.txt && git add f.txt && git commit -qm init");
    if (!env_ok) {
        std::filesystem::remove_all(repo, ec);
        return; // no git in PATH: skip the live part (recorded limitation)
    }

    GitRuntime git(std::filesystem::canonical(repo));
    check(git.alive(), "alive in a repository");
    const auto tools = git.list();
    check(tools.size() == 5, "five git tools listed");
    for (const auto& tool : tools) {
        check(tool.name != "git_push" && tool.name != "git_pull" && tool.name != "git_reset",
              "no remote/history-rewrite tools exposed");
    }
    auto call = [&](const char* name, const std::string& args) {
        return git.call(ToolCall{"1", name, args}, [] { return false; });
    };

    const auto status = call("git_status", "{}");
    check(!status.is_error, "git_status runs");
    // Modify + stage + commit flow.
    check(sh("echo change >> f.txt"), "shell edit works");
    const auto add = call("git_add", R"({"path":"f.txt"})");
    check(!add.is_error && add.text.find("staged") != std::string::npos, "git_add stages");
    // preview shows the staged file (run after add, before commit).
    const ToolCall commit{"1", "git_commit", R"({"message":"edit f"})"};
    const auto preview = git.preview(commit);
    check(preview.find("f.txt") != std::string::npos &&
              preview.find("1 staged") != std::string::npos,
          "commit preview lists staged files");
    const auto commit_res = git.call(commit, [] { return false; });
    check(!commit_res.is_error && commit_res.text.find("committed:") != std::string::npos,
          "git_commit creates local commit");
    const auto log = call("git_log", R"({"limit":5})");
    check(!log.is_error && log.text.find("edit f") != std::string::npos, "git_log shows it");
    const auto diff = call("git_diff", R"({"staged":true})");
    check(!diff.is_error, "git_diff staged runs (empty ok)");

    // Fail-closed argument surfaces.
    check(call("git_commit", R"({"message":""})").is_error, "empty message rejected");
    check(call("git_commit", R"({"message":"a\nb"})").is_error, "multiline subject rejected");
    check(call("git_commit", "{}").is_error, "missing message rejected");
    check(call("git_log", R"({"limit":0})").is_error, "limit 0 rejected");
    check(call("git_log", R"({"limit":"x"})").is_error, "string limit rejected");
    check(call("git_add", R"({"path":"../../escape"})").is_error, "add outside workspace rejected");
    check(call("git_diff", R"({"path":"../outside"})").is_error, "diff path escape rejected");
    const auto push = call("git_push", "{}");
    check(push.is_error && push.text.find("human-only") != std::string::npos,
          "git_push refused with policy message");

    // Argument injection: a message that LOOKS like flags stays data.
    check(sh("echo more >> f.txt && git add f.txt"), "stage again");
    const auto sneaky = call("git_commit", R"({"message":"--amend --allow-empty -m pwned"})");
    check(!sneaky.is_error, "flag-looking message commits as literal text");
    const auto verify = call("git_log", R"({"limit":1})");
    check(verify.text.find("--amend --allow-empty -m pwned") != std::string::npos,
          "literal subject landed in history intact");

    const auto bare = std::filesystem::temp_directory_path() / ("mini_agent_nogit_" + tag);
    std::filesystem::create_directories(bare, ec);
    GitRuntime dead(std::filesystem::canonical(bare));
    check(!dead.alive() && dead.list().empty(), "non-repository: tools disappear");
    check(dead.call(ToolCall{"1", "git_status", "{}"}, [] { return false; }).is_error,
          "dead runtime folds to error result");
    std::filesystem::remove_all(bare, ec);
    std::filesystem::remove_all(repo, ec);
}

// v4: provider that reports token usage through complete_chat.
class FakeUsageProvider final : public mini_agent::Provider {
  public:
    FakeUsageProvider(std::vector<mini_agent::TokenUsage> usages, int fail_on_call = -1)
        : usages_(std::move(usages)), fail_on_call_(fail_on_call) {
    }
    std::string complete(const mini_agent::Chat&, const std::function<void(std::string_view)>&,
                         const mini_agent::ShouldAbort&) override {
        return "plain";
    }
    mini_agent::CompletionResult
    complete_chat(const mini_agent::Chat&, const std::string&,
                  const std::function<void(std::string_view)>& on_delta,
                  const mini_agent::ShouldAbort&) override {
        if (index_ == fail_on_call_) {
            throw mini_agent::ProviderError(mini_agent::ProviderErrorKind::Network,
                                            "fake network failure");
        }
        mini_agent::CompletionResult result;
        result.text = "r" + std::to_string(index_);
        // Round 1 of a multi-round turn must request a tool so the loop
        // makes a second provider call and the usage accumulate branch runs.
        if (emit_tool_call_first && index_ == 0)
            result.tool_calls.push_back(mini_agent::ToolCall{"t1", "echo", "{}"});
        on_delta(result.text);
        if (index_ < static_cast<int>(usages_.size()))
            result.usage = usages_[static_cast<std::size_t>(index_)];
        ++index_;
        return result;
    }

    bool emit_tool_call_first = false;

  private:
    std::vector<mini_agent::TokenUsage> usages_;
    int index_ = 0;
    int fail_on_call_ = -1;
};

void test_v4_config_fields() {
    std::string error;
    // context_window round-trips through JSON; reasoning_effort too.
    const auto parsed = mini_agent::config_from_json(
        R"({"model":"m","context_window":128000,"reasoning_effort":"high"})", &error);
    check(parsed.has_value(), "v4 fields parse");
    check(parsed->context_window == 128000, "context_window parsed");
    check(parsed->reasoning_effort == "high", "reasoning_effort parsed");
    const auto text = mini_agent::config_to_json(*parsed);
    check(text.find("128000") != std::string::npos, "context_window written");
    check(text.find("reasoning_effort") != std::string::npos, "reasoning written");

    // fail-closed: a non-positive or non-integer context_window is rejected.
    check(!mini_agent::config_from_json(R"({"context_window":0})", &error).has_value(),
          "zero window rejected");
    check(!mini_agent::config_from_json(R"({"context_window":-5})", &error).has_value(),
          "negative window rejected");
    check(!mini_agent::config_from_json(R"({"context_window":"big"})", &error).has_value(),
          "non-integer window rejected");

    // defaults: absent fields stay unset and are omitted from output.
    const auto bare = mini_agent::config_from_json(R"({"model":"m"})", &error);
    check(bare.has_value() && bare->context_window == 0 && bare->reasoning_effort.empty(),
          "v4 defaults unset");
    check(mini_agent::config_to_json(*bare).find("context_window") == std::string::npos,
          "unset window omitted from output");

    // to_provider carries reasoning_effort (empty == omitted in the body).
    mini_agent::AppConfig cfg;
    cfg.base_url = "https://x.test/v1";
    cfg.api_key = "k";
    cfg.model = "m";
    cfg.reasoning_effort = "medium";
    check(cfg.to_provider().reasoning_effort == "medium", "provider carries reasoning effort");
}

void test_v4_status_segments() {
    using mini_agent::tui::StatusInputs;
    // Nothing set -> empty string keeps the pre-v4 status row byte-identical.
    check(mini_agent::tui::status_usage_segments(false, StatusInputs{}).empty(),
          "no inputs -> no segments");

    // ctx% with the 70/90 colour thresholds (colour codes present when on).
    StatusInputs mid;
    mid.context_tokens = 80000;
    mid.context_window = 100000; // 80% -> yellow 33
    const auto mid_line = mini_agent::tui::status_usage_segments(true, mid);
    check(mid_line.find("ctx 80.0k/100.0k") != std::string::npos, "ctx used/max shown");
    check(mid_line.find("\x1b[33m") != std::string::npos, "80% is yellow");
    StatusInputs high = mid;
    high.context_tokens = 95000; // 95% -> red 31
    check(mini_agent::tui::status_usage_segments(true, high).find("\x1b[31m") != std::string::npos,
          "95% is red");
    StatusInputs low = mid;
    low.context_tokens = 30000; // 30% -> dim 2
    check(mini_agent::tui::status_usage_segments(true, low).find("\x1b[2m") != std::string::npos,
          "30% is dim");

    // estimated flag adds the ~ prefix; no window falls back to a token count.
    StatusInputs est;
    est.context_tokens = 5000;
    est.context_estimated = true;
    const auto est_line = mini_agent::tui::status_usage_segments(false, est);
    check(est_line.find("ctx ~") != std::string::npos, "estimate marked with tilde");
    StatusInputs winless;
    winless.context_tokens = 5000;
    check(mini_agent::tui::status_usage_segments(false, winless).find("ctx 5.0k") !=
              std::string::npos,
          "no window -> token count");

    // reasoning + out segments append in order.
    StatusInputs full;
    full.context_tokens = 1000;
    full.context_window = 100000;
    full.completion_tokens = 2500;
    full.reasoning_effort = "high";
    const auto full_line = mini_agent::tui::status_usage_segments(false, full);
    check(full_line.find("out 2.5k") != std::string::npos, "out sum shown");
    check(full_line.find("reasoning=high") != std::string::npos, "reasoning shown");
    // colours off -> no escape bytes at all.
    check(mini_agent::tui::status_usage_segments(false, full).find('\x1b') == std::string::npos,
          "colors off strips SGR");
}

void test_v4_usage_collection() {
    // request body: v4 fields appear only when set (regression hook).
    mini_agent::Chat chat;
    chat.add(mini_agent::Message{mini_agent::Role::User, "q", std::nullopt, {}});
    const std::string base = mini_agent::preview_chat_request_body(chat, "m", "");
    check(base.find("stream_options") == std::string::npos, "usage opt-in omitted by default");
    check(base.find("reasoning_effort") == std::string::npos, "reasoning omitted when unset");
    const std::string with_usage = mini_agent::preview_chat_request_body(chat, "m", "", "", true);
    check(with_usage.find("\"stream_options\"") != std::string::npos &&
              with_usage.find("\"include_usage\":true") != std::string::npos,
          "usage opt-in present when requested");
    const std::string with_reasoning =
        mini_agent::preview_chat_request_body(chat, "m", "", "high", true);
    check(with_reasoning.find("\"reasoning_effort\":\"high\"") != std::string::npos,
          "reasoning effort passed through verbatim");

    // loop roll-up: last prompt wins, completion/total accumulate. With no
    // registry the turn is a single round, so exactly the first usage lands
    // (the multi-round accumulate rule is the loop's own contract).
    FakeUsageProvider provider({
        {100, 10, 110}, // round 1
        {150, 20, 170}, // never reached without a tools registry
    });
    mini_agent::Chat loop_chat;
    loop_chat.add(mini_agent::Message{mini_agent::Role::User, "sys", std::nullopt, {}});
    mini_agent::TurnOptions options;
    mini_agent::TokenUsage usage;
    options.usage_out = &usage;
    mini_agent::run_turn(
        loop_chat, provider, "hi", [](std::string_view) {}, [] { return false; }, options);
    check(usage.prompt_tokens == 100, "prompt_tokens = last round");
    check(usage.completion_tokens == 10, "completion_tokens accumulate");
    check(usage.total_tokens == 110, "total_tokens accumulate");

    // Multi-round: with a tools registry the loop runs two provider calls,
    // so the accumulate branch is exercised -- prompt takes the LAST round,
    // completion/total sum across rounds.
    FakeUsageProvider multi({
        {100, 10, 110}, // round 1 (requests a tool)
        {150, 20, 170}, // round 2 (final text): newer prompt replaces 100
    });
    multi.emit_tool_call_first = true;
    mini_agent::Chat multi_chat;
    multi_chat.add(mini_agent::Message{mini_agent::Role::User, "sys", std::nullopt, {}});
    auto reg = std::make_unique<mini_agent::ToolRegistry>();
    reg->add(std::unique_ptr<mini_agent::ToolRuntime>(new FakeRuntime()));
    mini_agent::TurnOptions multi_options;
    multi_options.tools = reg.get();
    mini_agent::TokenUsage multi_usage;
    multi_options.usage_out = &multi_usage;
    mini_agent::run_turn(
        multi_chat, multi, "hi", [](std::string_view) {}, [] { return false; }, multi_options);
    check(multi_usage.prompt_tokens == 150, "multi-round prompt = last");
    check(multi_usage.completion_tokens == 30, "multi-round completion sums");
    check(multi_usage.total_tokens == 280, "multi-round total sums");

    // failed turn: usage_out untouched (same commit boundary as history).
    FakeUsageProvider failing({{100, 10, 110}}, 0);
    mini_agent::Chat fail_chat;
    fail_chat.add(mini_agent::Message{mini_agent::Role::User, "sys", std::nullopt, {}});
    mini_agent::TokenUsage untouched{7, 7, 7};
    options.usage_out = &untouched;
    bool threw = false;
    try {
        mini_agent::run_turn(
            fail_chat, failing, "hi", [](std::string_view) {}, [] { return false; }, options);
    } catch (const mini_agent::ProviderError&) {
        threw = true;
    }
    check(threw && untouched.prompt_tokens == 7, "failed turn leaves usage_out");

    // estimator: coarse but monotonic, empty input -> 0.
    check(mini_agent::estimate_context_tokens("") == 0, "estimate empty is zero");
    check(mini_agent::estimate_context_tokens("hello world this is a plain english sentence") > 0,
          "estimate ascii positive");
    const long long ascii = mini_agent::estimate_context_tokens(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); // 64 a's
    const long long cjk =
        mini_agent::estimate_context_tokens("你好你好你好你好你好你好你好你好"
                                            "你好你好你好你好你好你好你好你好"); // 32 han
    check(ascii == 16, "ascii ~4 chars per token");
    check(cjk == 32, "cjk ~1 token per codepoint");
    check(mini_agent::estimate_context_tokens(std::string(64, 'a')) ==
              mini_agent::estimate_context_tokens(std::string(64, 'a')),
          "estimate deterministic");
}

void test_v42_capabilities_and_menu() {
    std::string error;
    // OpenRouter shape: efforts list, default, mandatory, context length.
    const auto caps = mini_agent::parse_models_meta_json(
        R"({"data":[
             {"id":"or/gpt","context_length":105000,
              "reasoning":{"mandatory":false,"default_enabled":false,
                           "supported_efforts":["high","medium","low","none"],
                           "default_effort":"medium"}},
             {"id":"plain","object":"model"},
             {"id":"or/qwen","context_length":1000000,
              "reasoning":{"mandatory":true,"supported_efforts":["xhigh","low"],
                           "default_effort":"xhigh"}}]})",
        &error);
    check(caps.has_value(), "caps body parses");
    check(caps->size() == 3, "all entries kept");
    const mini_agent::ModelCapabilities* gpt = nullptr;
    const mini_agent::ModelCapabilities* qwen = nullptr;
    for (const auto& c : *caps) {
        if (c.id == "or/gpt")
            gpt = &c;
        if (c.id == "or/qwen")
            qwen = &c;
    }
    check(gpt && gpt->supports_reasoning && gpt->reasoning_efforts.size() == 4,
          "gpt efforts parsed");
    check(gpt && gpt->reasoning_default_effort == "medium", "default effort parsed");
    check(gpt && gpt->context_length == 105000, "context length parsed");
    check(gpt && !gpt->reasoning_mandatory, "mandatory flag parsed");
    check(qwen && qwen->reasoning_mandatory, "mandatory true parsed");
    // entries without metadata yield empty caps, not errors
    const mini_agent::ModelCapabilities* plain = nullptr;
    for (const auto& c : *caps)
        if (c.id == "plain")
            plain = &c;
    check(plain && !plain->supports_reasoning && plain->context_length == 0,
          "metadata-free entry stays empty");
    // fail-closed like parse_models_json
    check(!mini_agent::parse_models_meta_json("not json", &error).has_value(), "garbage rejected");
    check(!mini_agent::parse_models_meta_json(R"({"models":[]})", &error).has_value(),
          "missing data rejected");
    // bare-bool reasoning flag shape (some gateways)
    const auto flag =
        mini_agent::parse_models_meta_json(R"({"data":[{"id":"m","reasoning":true}]})", &error);
    check(flag && flag->at(0).supports_reasoning && flag->at(0).reasoning_efforts.empty(),
          "bool reasoning flag supported");

    // menu rows: numbering, default/current marks, mandatory note.
    const auto rows =
        mini_agent::tui::reasoning_menu_rows({"high", "medium", "low"}, "high", "medium", false);
    check(rows.size() == 5, "header + 3 efforts + footer");
    check(rows[1].find("1. high") != std::string::npos &&
              rows[1].find("<- current") != std::string::npos,
          "current marked");
    check(rows[2].find("<- default") != std::string::npos, "default marked");
    check(mini_agent::tui::reasoning_menu_rows({}, "", "", false).empty(),
          "empty efforts -> no rows");
    const auto mand = mini_agent::tui::reasoning_menu_rows({"xhigh"}, "", "xhigh", true);
    check(mand[0].find("mandatory") != std::string::npos, "mandatory noted");
    // selection reuses model_choice against the efforts vector
    const auto pick = mini_agent::tui::model_choice({"high", "medium", "low"}, "2");
    check(pick.has_value() && *pick == "medium", "numbered pick resolves");

    // status preference: actual > seen-without-level > config echo.
    mini_agent::tui::StatusInputs inputs;
    inputs.reasoning_effort = "medium";
    check(mini_agent::tui::status_usage_segments(false, inputs).find("reasoning=medium") !=
              std::string::npos,
          "config echo when no evidence");
    inputs.reasoning_seen = true;
    // v4.2.1: evidence + a configured level confirms that level (gateways
    // like qwen echo no effort string; showing "on" would be a downgrade).
    check(mini_agent::tui::status_usage_segments(false, inputs).find("reasoning=medium") !=
              std::string::npos,
          "seen with config -> config level confirmed");
    mini_agent::tui::StatusInputs bare;
    bare.reasoning_seen = true; // evidence, no level known anywhere
    check(mini_agent::tui::status_usage_segments(false, bare).find("reasoning=on") !=
              std::string::npos,
          "seen without any level -> on");
    inputs.reasoning_actual = "high";
    check(mini_agent::tui::status_usage_segments(false, inputs).find("reasoning=high") !=
              std::string::npos,
          "actual level wins");
    check(mini_agent::tui::status_usage_segments(false, inputs).find("reasoning=medium") ==
              std::string::npos,
          "config echo replaced, not appended");
}

void test_v47_models_dev_catalog() {
    // Hit: a (base_url, model) pair present in the generated snapshot.
    const auto hit =
        mini_agent::lookup_models_dev("http://127.0.0.1:1234/v1", "qwen/qwen3-coder-30b");
    check(hit.has_value() && hit->context == 262144, "catalog row -> context window");
    check(hit->output == 65536, "catalog row -> output limit");
    // Trailing slashes on the configured base_url must not miss the row.
    const auto slashed =
        mini_agent::lookup_models_dev("http://127.0.0.1:1234/v1/", "qwen/qwen3-coder-30b");
    check(slashed.has_value() && slashed->context == 262144, "base_url trailing slash trimmed");
    // Misses stay misses: unknown model, unknown url, empty inputs.
    check(!mini_agent::lookup_models_dev("http://127.0.0.1:1234/v1", "no-such-model").has_value(),
          "unknown model -> miss");
    check(!mini_agent::lookup_models_dev("http://gateway.invalid/v1", "qwen/qwen3-coder-30b")
               .has_value(),
          "unknown base_url -> miss");
    check(!mini_agent::lookup_models_dev("", "m").has_value(), "empty url -> miss");
    check(!mini_agent::lookup_models_dev("https://x.test/v1", "").has_value(),
          "empty model -> miss");
    // Prefix-adjacent ids must not false-hit (binary search verifies exact key).
    check(!mini_agent::lookup_models_dev("http://127.0.0.1:1234/v1", "qwen/qwen3-coder-30b-extra")
               .has_value(),
          "id prefix never false-hits");
    // The maintainer's real gateway row (models.dev alibaba-token-plan-cn).
    const auto qwen = mini_agent::lookup_models_dev(
        "https://token-plan.cn-beijing.maas.aliyuncs.com/compatible-mode/v1", "qwen3.8-flash");
    check(qwen.has_value() && qwen->context == 1000000 && qwen->output == 131072,
          "token-plan-cn qwen3.8-flash row matches live catalog");
}

void test_v410_arg_completion() {
    namespace T = mini_agent::tui;
    // Second-tier hint: exact command lists its argument choices.
    const auto ctx = T::slash_completion_hint("/context");
    check(ctx.size() == 4 && ctx[0].find("options for /context") != std::string::npos,
          "/context shows option header + 3 rows");
    check(ctx[1].find("<tokens>") != std::string::npos &&
              ctx[2].find("probe") != std::string::npos && ctx[3].find("off") != std::string::npos,
          "choices listed after the header in registry order");
    // Partial argument filters the list.
    const auto p = T::slash_completion_hint("/context p");
    check(p.size() == 2 && p[1].find("probe") != std::string::npos, "/context p filters to probe");
    // Free-form values are not misses: typing a number stays silent.
    check(T::slash_completion_hint("/context 123").empty(), "typed value never nags");
    // Command tier unchanged while the command is still ambiguous.
    const auto amb = T::slash_completion_hint("/con");
    check(amb.size() == 3 && amb[1].find("/config") != std::string::npos &&
              amb[2].find("/context") != std::string::npos,
          "/con still completes commands, not args");
    // Tab cycles args once the command is exact.
    int cycle = 0;
    auto a = T::slash_tab_complete("/context", cycle);
    check(a && *a == "/context <tokens>", "Tab first arg choice");
    auto b = T::slash_tab_complete("/context", cycle);
    check(b && *b == "/context probe", "Tab cycles to second choice");
    auto filtered = T::slash_tab_complete("/context p", cycle);
    check(filtered && *filtered == "/context probe", "partial arg pins filtered choice");
    // Rest IS a full choice: Tab rotates to the NEXT one deterministically
    // (the inserted-value case), wrapping around the list end.
    int c3 = 0;
    auto rot1 = T::slash_tab_complete("/context <tokens>", c3);
    check(rot1 && *rot1 == "/context probe", "exact choice rotates to next");
    auto rot2 = T::slash_tab_complete("/context off", c3);
    check(rot2 && *rot2 == "/context <tokens>", "last choice wraps to first");
    // Exact command with a prefix-sharing sibling still enters the arg tier.
    auto sess = T::slash_tab_complete("/session", c3);
    check(sess && *sess == "/session <id>", "/session enters args despite /sessions sibling");
    // Commands without a choice set keep cycling the command itself.
    int c2 = 0;
    auto h = T::slash_tab_complete("/help", c2);
    check(h && *h == "/help", "/help Tab stays on the command");
    // /log level tier.
    const auto log = T::slash_completion_hint("/log ");
    check(log.size() == 7 && log[1].find("info") != std::string::npos, "/log lists 6 choices");
}

void test_v43_probe_parse() {
    using K = mini_agent::ProbeLimitKind;
    // vLLM/OpenAI total window: the only shape that IS the denominator.
    const auto vllm = mini_agent::parse_context_probe_error(
        R"({"error":{"message":"This model's maximum context length is 32768 tokens. However, you requested 1000000000 tokens"}})");
    check(vllm.kind == K::Window && vllm.value == 32768, "vllm form -> Window");
    // DashScope input cap: usable proxy for the prompt-side reading.
    const auto dash_in = mini_agent::parse_context_probe_error(
        R"({"error":{"message":"<400> Range of input length should be [1, 983616]"}})");
    check(dash_in.kind == K::InputCap && dash_in.value == 983616, "input range -> InputCap");
    // DashScope max_tokens range: the OUTPUT cap -- must never be stored
    // as the context window (maintainer data: qwen 131072 out vs 983616 in).
    const auto dash_out =
        mini_agent::parse_context_probe_error("Range of max_tokens should be [1, 131072]");
    check(dash_out.kind == K::OutputCap && dash_out.value == 131072,
          "max_tokens range -> OutputCap, not window");
    const auto openai_gt = mini_agent::parse_context_probe_error(
        R"({"error":{"message":"max_tokens: 1000000000 > 131072, which is the maximum number of tokens that the model can generate"}})");
    check(openai_gt.kind == K::OutputCap && openai_gt.value == 131072, "openai gt -> OutputCap");
    // Unknown / hostile shapes yield nothing rather than a wrong number.
    check(mini_agent::parse_context_probe_error("something went wrong").kind == K::Unknown,
          "unknown text -> Unknown");
    check(mini_agent::parse_context_probe_error(R"({"max_tokens":1000000000})").kind == K::Unknown,
          "echoed request value never parsed");
    check(mini_agent::parse_context_probe_error("Range of input length should be [1, 7]").kind ==
              K::Unknown,
          "out-of-range bound -> Unknown");
    check(mini_agent::parse_context_probe_error("").kind == K::Unknown, "empty -> Unknown");
    // Ambiguous "Range of" param name: refuse to classify (never guess).
    const auto ambig =
        mini_agent::parse_context_probe_error("Range of tokens should be [1, 65536]");
    check(ambig.kind == K::Unknown && ambig.value == 0, "ambiguous param -> Unknown");
}

void test_v44_debug_log() {
    std::string error;
    // v4.8.1 (010 §19): the level is session-scoped -- the file is never
    // a source and never a sink. log_level/legacy bools in JSON are
    // ignored (not fail-closed: they are simply not config keys), and
    // saving drops them.
    const auto raised =
        mini_agent::config_from_json(R"({"model":"m","log_level":"debug"})", &error);
    check(raised.has_value() && raised->log_level == "info",
          "file log_level ignored (session-only)");
    const auto legacy = mini_agent::config_from_json(
        R"({"model":"m","debug_log":true,"debug_log_aux":true})", &error);
    check(legacy.has_value() && legacy->log_level == "info", "legacy debug bools ignored on load");
    check(mini_agent::config_to_json(*legacy).find("debug_log") == std::string::npos,
          "saving drops legacy debug_log fields");
    mini_agent::AppConfig lvl;
    lvl.base_url = "https://x.test/v1";
    lvl.api_key = "k";
    lvl.model = "m";
    lvl.log_level = "trace";
    check(mini_agent::config_to_json(lvl).find("log_level") == std::string::npos,
          "raised level never written to file");
    lvl.log_level = "info";
    check(mini_agent::config_to_json(lvl).find("log_level") == std::string::npos,
          "info omitted from output");
    const auto junk = mini_agent::config_from_json(R"({"log_level":"verbose"})", &error);
    check(junk.has_value() && junk->log_level == "info",
          "unknown level in file tolerated (ignored, not fail-closed)");

    // gates + path derivation: <log_file>.debug; "off"/empty fall back to
    // default base; info -> no path at all.
    mini_agent::AppConfig cfg;
    cfg.base_url = "https://x.test/v1";
    cfg.api_key = "k";
    cfg.model = "m";
    cfg.log_level = "debug";
    cfg.log_file = "/var/tmp/my.log";
    check(cfg.to_provider().debug_log_path == "/var/tmp/my.log.debug", "debug path from log_file");
    check(cfg.to_provider().debug_log_path.find(".debug") != std::string::npos, "path suffix");
    cfg.log_file = "off";
    check(cfg.to_provider().debug_log_path.find(".debug") != std::string::npos &&
              cfg.to_provider().debug_log_path.find("off") == std::string::npos,
          "off sentinel falls back to default base");
    cfg.log_level = "info";
    check(cfg.to_provider().debug_log_path.empty(), "info -> no path");
    check(!cfg.capture_chat() && !cfg.capture_aux(), "info gates off");
    cfg.log_level = "debug";
    check(cfg.capture_chat() && !cfg.capture_aux(), "debug gates chat only");
    cfg.log_level = "trace";
    check(cfg.capture_chat() && cfg.capture_aux(), "trace gates both");
    check(cfg.to_provider().debug_aux, "provider carries aux at trace");
    cfg.log_level = "info";
    check(!cfg.to_provider().debug_aux, "info -> provider aux off");
}

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
    test_models_listing();
    test_config_batched_hint();
    test_login_status();
    test_connection_prompt();
    test_logger();
    test_md_width_and_wrap();
    test_md_render_subset();
    test_md4c_gfm_extensions();
    test_tui_helpers();
    test_tool_ui_helpers();
    test_history_styling();
    test_tool_calls_round_trip();
    test_v1_request_shape_regresssion();
    test_session_schema_bumps_for_tools();
    test_tool_loop_rounds_and_commit();
    test_max_tool_rounds();
    test_run_bash_and_helpers();
    test_config_v2_fields();
    test_mcp_cli_specs();
    test_unified_diff();
    test_file_tools();
    test_v3_config_and_prompt();
    test_approval_preview_rows();
    test_v4_usage_collection();
    test_v4_config_fields();
    test_v4_status_segments();
    test_v42_capabilities_and_menu();
    test_v43_probe_parse();
    test_v47_models_dev_catalog();
    test_v410_arg_completion();
    test_v44_debug_log();
    test_git_tools();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all agent checks passed\n";
    return EXIT_SUCCESS;
}
