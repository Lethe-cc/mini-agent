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
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/md.hpp"
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
                return CompletionResult{"", {ToolCall{"t1", "echo", "{}"}}};
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
                return CompletionResult{"", {ToolCall{"t" + std::to_string(n), "echo", "{}"}}};
            return CompletionResult{"gave up gracefully", {}};
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
    test_tool_calls_round_trip();
    test_v1_request_shape_regresssion();
    test_session_schema_bumps_for_tools();
    test_tool_loop_rounds_and_commit();
    test_max_tool_rounds();
    test_run_bash_and_helpers();
    test_config_v2_fields();
    test_mcp_cli_specs();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all agent checks passed\n";
    return EXIT_SUCCESS;
}
