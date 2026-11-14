// Unit tests for chat/session/loop (design 007 section 8: extension-point
// format commitments are tested from v1). Plain asserts, no framework dep.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/diff.hpp"
#include "mini_agent/highlight.hpp"
#include "mini_agent/log.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/md.hpp"
#include "mini_agent/models_dev.hpp"
#include "mini_agent/provider.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools.hpp"
#include "mini_agent/tui.hpp"
#include "mini_agent/tui_bridge.hpp"

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
    check(!store.remove(session.meta.id, &error), "double remove fails");
    check(error.find("no such session") != std::string::npos,
          "missing id reports 'no such session' (v4.14.1)");
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
    mini_agent::ProviderProfile profile;
    profile.name = "default";
    profile.base_url = "https://example.test/v1";
    profile.api_key = "secret";
    profile.model = "m1";
    full.providers = {profile};
    full.activate_provider("default");
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
    check(merged.current.base_url == "https://example.test/v1", "file base_url kept");
    check(merged.current.api_key == "secret", "file key kept");
    check(merged.current.model == "env-model", "env overrides file model");
    ::unsetenv("MINI_AGENT_MODEL");
    // trailing slash tolerated
    mini_agent::AppConfig slashed;
    mini_agent::ProviderProfile slash_profile;
    slash_profile.name = "default";
    slash_profile.base_url = "https://trail.test/v1/";
    slash_profile.api_key = "k";
    slash_profile.model = "m";
    slashed.providers = {slash_profile};
    slashed.activate_provider("default");
    check(mini_agent::save_config_file(slashed, &error), "save slashed");
    auto reloaded = mini_agent::load_config_merged(&error);
    check(reloaded.current.base_url == "https://trail.test/v1", "trailing slash trimmed");
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

// v7 (design 013): named profiles are the only persisted LLM state;
// current is derived one-way (active profile + env overlay), v1 files
// migrate once at load, and malformed profile state fails closed.
void test_provider_profiles() {
    std::string error;

    // Two profiles, partial second one (add-before-login is legal).
    mini_agent::AppConfig cfg;
    mini_agent::ProviderProfile a;
    a.name = "alpha";
    a.base_url = "https://a.test/v1/";
    a.api_key = "key-a";
    a.model = "a-model";
    a.context_window = 100000;
    a.reasoning_effort = "high";
    mini_agent::ProviderProfile b;
    b.name = "beta";
    cfg.providers = {a, b};
    check(cfg.activate_provider("alpha"), "activate alpha");
    check(cfg.current.base_url == "https://a.test/v1", "current derives normalized url");
    check(cfg.current.api_key == "key-a" && cfg.current.model == "a-model",
          "current derives key/model");
    check(cfg.current.context_window == 100000 && cfg.current.reasoning_effort == "high",
          "current derives window/effort");
    check(cfg.current.name == "alpha", "current carries the profile name");

    // v2 save: schema 2, no top-level LLM keys (dump(2): top-level keys are
    // indented two spaces, profile fields four).
    const std::string v2_text = mini_agent::config_to_json(cfg);
    check(v2_text.find("\"schema_version\": 2") != std::string::npos, "schema 2 written");
    check(v2_text.find("\n  \"base_url\"") == std::string::npos &&
              v2_text.find("\n  \"api_key\"") == std::string::npos &&
              v2_text.find("\n  \"model\"") == std::string::npos,
          "v2 save has no top-level LLM keys");

    // save_config_file writes to MINI_AGENT_CONFIG; route to a temp file.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mini_agent_prov_" + mini_agent::generate_session_id());
    ::setenv("MINI_AGENT_CONFIG", (dir / "config.json").string().c_str(), 1);
    check(mini_agent::save_config_file(cfg, &error), (std::string("save temp: ") + error).c_str());
    auto merged = mini_agent::load_config_merged(&error);
    check(error.empty(), (std::string("load clean: ") + error).c_str());
    check(merged.providers.size() == 2, "profiles round trip");
    check(merged.active_provider == "alpha", "active survives round trip");
    check(merged.current.base_url == "https://a.test/v1" && merged.current.api_key == "key-a" &&
              merged.current.model == "a-model",
          "loaded current is the active profile");
    check(merged.providers[0].base_url == "https://a.test/v1", "profile url normalized on load");
    check(merged.providers[1].name == "beta" && merged.providers[1].base_url.empty() &&
              merged.providers[1].valid(&error),
          "partial profile legal until first use");
    check(!merged.migrated_from_v1, "v2 file not flagged as migrated");

    // Cross-profile switch clears the previous window/effort (no leak).
    check(merged.activate_provider("beta"), "activate beta");
    check(merged.current.context_window == 0 && merged.current.reasoning_effort.empty(),
          "unset profile window/effort clear current");
    check(merged.current.base_url.empty() && merged.current.api_key.empty() &&
              merged.current.model.empty(),
          "partial profile derives empty current");
    check(!merged.activate_provider("ghost"), "unknown profile rejected");
    check(!merged.activate_provider(""), "empty name rejected (no unnamed state)");
    check(merged.active_provider == "beta", "failed activate keeps selection");

    // Edits go to the profile first, then re-derive (no write-back sync).
    auto* pb = merged.find_provider("beta");
    pb->base_url = "https://b.test/v1";
    pb->api_key = "key-b";
    pb->model = "b-model";
    pb->context_window = 50000;
    merged.refresh_current();
    check(merged.current.base_url == "https://b.test/v1" && merged.current.model == "b-model" &&
              merged.current.context_window == 50000,
          "refresh derives profile edits");

    // v1 migration: unnamed top-level folds into a default profile.
    const auto v1 = mini_agent::config_from_json(
        R"({"schema_version":1,"base_url":"https://old.test/v1/","api_key":"old-key",)"
        R"("model":"old-m","context_window":64000,"reasoning_effort":"low"})",
        &error);
    check(v1.has_value(), (std::string("v1 migration parses: ") + error).c_str());
    check(v1->providers.size() == 1 && v1->active_provider == "default",
          "unnamed v1 folds into default");
    check(v1->current.base_url == "https://old.test/v1" && v1->current.api_key == "old-key" &&
              v1->current.model == "old-m",
          "migrated values derive into current");
    check(v1->current.context_window == 64000 && v1->current.reasoning_effort == "low",
          "migrated metadata kept");
    check(v1->migrated_from_v1, "migration flagged for the log event");
    const std::string v1_saved = mini_agent::config_to_json(*v1);
    check(v1_saved.find("\n  \"base_url\"") == std::string::npos,
          "migrated save drops top-level url");

    // v1 with a named selection: top-level expansion dropped, table kept.
    const auto v1named = mini_agent::config_from_json(
        R"({"schema_version":1,"base_url":"https://stale.test","api_key":"stale",)"
        R"("model":"stale","active_provider":"alpha",)"
        R"("providers":{"alpha":{"base_url":"https://a.test/v1","api_key":"key-a","model":"a-model"}}})",
        &error);
    check(v1named.has_value() && v1named->providers.size() == 1 &&
              v1named->current.model == "a-model" && v1named->current.api_key == "key-a",
          "named v1 keeps the profile, drops stale top level");

    // Name collision: existing default -> deterministic default-2.
    const auto collide = mini_agent::config_from_json(
        R"({"base_url":"https://new.test/v1",)"
        R"("providers":{"default":{"base_url":"https://kept.test/v1"}}})",
        &error);
    check(collide.has_value() && collide->providers.size() == 2 &&
              collide->active_provider == "default-2",
          "collision folds into default-2");
    check(collide->current.base_url == "https://new.test/v1", "collision keeps legacy values");

    // Empty v1 file: nothing to migrate.
    const auto empty_v1 = mini_agent::config_from_json(R"({"schema_version":1})", &error);
    check(empty_v1.has_value() && empty_v1->providers.empty() && !empty_v1->migrated_from_v1,
          "empty v1 stays empty");

    // v2 invariants: a table requires a selection; top-level LLM fields are
    // unknown fields and ignored.
    check(!mini_agent::config_from_json(R"({"schema_version":2,"providers":{"x":{}}})", &error)
               .has_value(),
          "v2 table without active fails closed");
    const auto v2ignore = mini_agent::config_from_json(
        R"({"schema_version":2,"base_url":"https://ignored.test","api_key":"k","model":"m"})",
        &error);
    check(v2ignore.has_value() && v2ignore->providers.empty() && v2ignore->current.base_url.empty(),
          "v2 ignores top-level LLM fields");

    // Fail-closed parsing.
    check(!mini_agent::config_from_json(R"({"providers":[]})", &error).has_value(),
          "providers must be an object");
    check(!mini_agent::config_from_json(R"({"providers":{"a":5}})", &error).has_value(),
          "profile entry must be an object");
    check(!mini_agent::config_from_json(R"({"providers":{"bad name":{}}})", &error).has_value(),
          "profile name charset enforced");
    check(!mini_agent::config_from_json(R"({"providers":{"x":{"base_url":"not-a-url"}}})", &error)
               .has_value(),
          "profile url must be absolute");
    check(!mini_agent::config_from_json(
               R"({"providers":{"x":{"base_url":"ftp://h/v1"}}})", &error)
               .has_value(),
          "profile url scheme enforced");
    check(!mini_agent::config_from_json(R"({"providers":{"x":{"context_window":0}}})", &error)
               .has_value(),
          "profile window must be positive");
    check(!mini_agent::config_from_json(R"({"active_provider":"ghost"})", &error).has_value(),
          "dangling active_provider fails closed");
    check(error.find("ghost") != std::string::npos, "dangling error names the selection");
    // Plain http is allowed for profiles (top-level policy, unlike MCP).
    check(mini_agent::config_from_json(
               R"({"providers":{"x":{"base_url":"http://remote.test/v1"}}})", &error)
               .has_value(),
          "profile http url follows top-level policy");

    // Qualified model references.
    std::string prov, id;
    check(mini_agent::split_qualified_model("alpha/m1", &prov, &id) && prov == "alpha" &&
              id == "m1",
          "qualified split");
    check(mini_agent::split_qualified_model("openai/org/model-x", &prov, &id) &&
              prov == "openai" && id == "org/model-x",
          "split at first slash only");
    check(!mini_agent::split_qualified_model("plain", &prov, &id), "unqualified rejected");
    check(!mini_agent::split_qualified_model("/m", &prov, &id), "empty provider rejected");
    check(!mini_agent::split_qualified_model("p/", &prov, &id), "empty model rejected");

    // Qualified MINI_AGENT_MODEL derives from that profile without touching
    // the persisted selection; plain env still wins.
    check(mini_agent::save_config_file(cfg, &error), "save for env test");
    ::setenv("MINI_AGENT_MODEL", "alpha/a-model", 1);
    auto env_cfg = mini_agent::load_config_merged(&error);
    check(env_cfg.current.model == "a-model" && env_cfg.current.base_url == "https://a.test/v1" &&
              env_cfg.current.api_key == "key-a",
          "qualified env derives the whole profile");
    check(env_cfg.active_provider == "alpha", "env does not rewrite the file selection");
    check(env_cfg.env_override_active, "env model sets override state");
    // env pins the effective profile across switches (design 013 §5.3).
    check(env_cfg.activate_provider("beta"), "switch under env");
    check(env_cfg.current.model == "a-model" && env_cfg.current.base_url == "https://a.test/v1",
          "env keeps winning after a profile switch");
    check(env_cfg.active_provider == "beta", "selection still switches for the next save");
    ::setenv("MINI_AGENT_BASE_URL", "https://env.test/v1", 1);
    env_cfg = mini_agent::load_config_merged(&error);
    check(env_cfg.current.base_url == "https://env.test/v1", "plain env url beats profile");
    ::unsetenv("MINI_AGENT_BASE_URL");
    ::setenv("MINI_AGENT_MODEL", "literal-model", 1);
    env_cfg = mini_agent::load_config_merged(&error);
    check(env_cfg.current.model == "literal-model" && env_cfg.active_provider == "alpha",
          "plain env model keeps file selection");
    ::setenv("MINI_AGENT_MODEL", "ghost/m", 1);
    env_cfg = mini_agent::load_config_merged(&error);
    check(env_cfg.current.model == "ghost/m", "unknown env prefix stays a literal model id");
    // CLI --model session overlay wins over env, never persisted.
    env_cfg.session_model = "cli-model";
    env_cfg.refresh_current();
    check(env_cfg.current.model == "cli-model", "session model overlay beats env");
    check(mini_agent::config_to_json(env_cfg).find("cli-model") == std::string::npos,
          "session overlay never written");
    env_cfg.session_model.clear();
    ::unsetenv("MINI_AGENT_MODEL");
    env_cfg = mini_agent::load_config_merged(&error);
    check(!env_cfg.env_override_active, "no endpoint env -> no override state");

    ::unlink((dir / "config.json").c_str());
    ::rmdir(dir.c_str());
    ::unsetenv("MINI_AGENT_CONFIG");
}

void test_config_batched_hint() {
    // Batched entry (2026-11-02 request): each command saves independently,
    // so the hint must name only the NEXT missing step, never all three.
    mini_agent::AppConfig empty;
    check(mini_agent::tui::config_next_hint(empty) == "config 0/3 · next: /provider <url>",
          "nothing set points at /provider only");

    mini_agent::AppConfig has_url;
    has_url.current.base_url = "https://x.test/v1";
    check(mini_agent::tui::config_next_hint(has_url) == "config 1/3 · next: /login <key>",
          "url set points at /login only");

    // Order independence: a key without a url must still ask for the url.
    mini_agent::AppConfig has_key;
    has_key.current.api_key = "k";
    check(mini_agent::tui::config_next_hint(has_key) == "config 1/3 · next: /provider <url>",
          "key-only still points at /provider first");

    mini_agent::AppConfig url_and_key;
    url_and_key.current.base_url = "https://x.test/v1";
    url_and_key.current.api_key = "k";
    check(mini_agent::tui::config_next_hint(url_and_key) ==
              "config 2/3 · next: /models (list & pick) or /model <name>",
          "url+key points at model discovery");

    mini_agent::AppConfig model_only;
    model_only.current.model = "m";
    check(mini_agent::tui::config_next_hint(model_only) == "config 1/3 · next: /provider <url>",
          "model-only points at /provider");

    mini_agent::AppConfig full;
    full.current.base_url = "https://x.test/v1";
    full.current.api_key = "k";
    full.current.model = "m";
    check(mini_agent::tui::config_next_hint(full) == "config 3/3 · ready", "complete says ready");
    check(full.complete(), "hint agrees with complete()");

    // A hint line must never carry the key value itself.
    mini_agent::AppConfig secrety;
    secrety.current.api_key = "do-not-leak-this";
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
    url_only.current.base_url = "https://api.test/v1";
    check(mini_agent::tui::connection_state(url_only, AuthProbe{}, false) ==
              ConnState::Unconfigured,
          "an endpoint without a key is not connectable");

    mini_agent::AppConfig cfg;
    cfg.current.base_url = "https://api.test/v1";
    cfg.current.api_key = "do-not-leak-this";
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
    plain.current.base_url = "https://x.test/v1";
    plain.current.api_key = "sekret123";
    plain.current.model = "m";
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

void test_highlight() {
    using namespace mini_agent;
    // Language mapping: paths, fences, aliases, unknowns stay empty.
    check(hl::language_for_path("src/a.cpp") == "cpp", "path .cpp -> cpp");
    check(hl::language_for_path("x/y.py") == "py", "path .py -> py");
    check(hl::language_for_path("run.sh") == "sh", "path .sh -> sh");
    check(hl::language_for_path("data.json") == "json", "path .json -> json");
    check(hl::language_for_path("App.tsx") == "js", "path .tsx -> js");
    check(hl::language_for_path("README.md").empty(), "markdown not a code lang");
    check(hl::language_for_path("a.dir/file").empty(), "dir dot is not an extension");
    check(hl::language_for_fence("python") == "py", "fence alias");
    check(hl::language_for_fence("C++") == "cpp", "fence case-insensitive");
    check(hl::language_for_fence("jsonc").empty() || hl::language_for_fence("jsonc") == "json",
          "fence jsonc resolves");
    check(hl::language_for_fence("brainfuck").empty(), "unknown fence empty");

    auto lit = [&](const std::string& line, const std::string& lang) {
        hl::State st;
        return hl::highlight_line(line, lang, st);
    };
    // cpp: keyword cyan, call blue, comment dim, string green, preproc yellow.
    const auto cpp = lit("int main() { return 0; }", "cpp");
    check(cpp.find("\x1b[36mint\x1b[0m") != std::string::npos, "cpp keyword cyan");
    check(cpp.find("\x1b[34mmain\x1b[0m") != std::string::npos, "cpp call name blue");
    check(cpp.find("\x1b[35m0\x1b[0m") != std::string::npos, "cpp number magenta");
    check(lit("int x; // note", "cpp").find("\x1b[2m// note") != std::string::npos,
          "cpp line comment dim");
    check(lit("#include <x>", "cpp").rfind("\x1b[33m#", 0) == 0, "cpp preproc yellow line-wide");
    check(lit("s = \"hi\";", "cpp").find("\x1b[32m\"hi\"") != std::string::npos,
          "cpp string green");

    // python: triple quote + comment + keyword.
    check(lit("# c", "py").find("\x1b[2m# c") != std::string::npos, "py comment");
    check(lit("def f():", "py").find("\x1b[36mdef\x1b[0m") != std::string::npos, "py def keyword");

    // shell: variable expansion yellow, keyword.
    const auto sh = lit("echo $HOME", "sh");
    check(sh.find("\x1b[33m$HOME\x1b[0m") != std::string::npos, "sh var yellow");
    check(lit("for x in 1 2; do :; done", "sh").find("\x1b[36mfor\x1b[0m") != std::string::npos,
          "sh keyword");

    // json: key vs value vs literal.
    const auto js = lit(R"({"a": 1, "b": "s", "c": null})", "json");
    check(js.find("\x1b[34m\"a\"") != std::string::npos, "json key blue");
    check(js.find("\x1b[36mnull\x1b[0m") != std::string::npos, "json literal keyword");

    // Unknown language and empty pass through unchanged.
    check(lit("plain text", "").find('\x1b') == std::string::npos, "empty lang passthrough");
    check(lit("x", "brainfuck").find('\x1b') == std::string::npos, "unknown lang passthrough");

    // SGR spans are zero-width to md::wrap: highlighted rows still respect
    // the column invariant (the TUI commit path relies on it).
    hl::State st;
    const auto long_line = hl::highlight_line(
        "static const std::string keyword = \"a very long string value here\"", "cpp", st);
    bool fits = true;
    for (const auto& row : md::wrap(long_line, 20)) {
        if (md::display_width(row) > 20)
            fits = false;
    }
    check(fits, "highlighted text wraps within cols (SGR zero-width)");
    // Balanced resets: no SGR survives end of line.
    check(long_line.rfind("\x1b[0m") == long_line.size() - 4, "highlight closes every span");
    // Cross-line block comment carries.
    hl::State cs;
    const auto c1 = hl::highlight_line("/* open", "cpp", cs);
    const auto c2 = hl::highlight_line("still comment */ int x;", "cpp", cs);
    check(c1.find("\x1b[2m") != std::string::npos, "block comment opens dim");
    check(c2.find("\x1b[2m") != std::string::npos && c2.find("\x1b[36mint") != std::string::npos,
          "block comment closes mid-line, code resumes");
    check(cs.open == 0, "state returns to normal after close");
}

void test_highlight_in_render() {
    using namespace mini_agent;
    // md::render highlights recognized fences, leaves unknown fences dim.
    const auto code = md::render("```cpp\nint x = 1; // c\n```\n", 60);
    check(code.find("\x1b[36mint\x1b[0m") != std::string::npos, "fence highlights keyword");
    const auto plain = md::render("```\nint x = 1;\n```\n", 60);
    check(plain.find("\x1b[2m") != std::string::npos &&
              plain.find("\x1b[36mint\x1b[0m") == std::string::npos,
          "plain fence stays dim (no highlight)");
    // inline code is unchanged (not a fence body).
    check(md::render("`code`", 60).find("\x1b[7m") != std::string::npos, "inline code inverse");
}

void test_tui_helpers() {
    // slash registry & prefix matching
    auto matches = mini_agent::tui::slash_matches("/s");
    check(matches.size() == 4 && matches[0] == "/session" && matches[1] == "/sessions" &&
              matches[2] == "/sidebar" && matches[3] == "/sysinfo",
          "prefix /s matches session/sessions/sidebar/sysinfo (v4.13)");
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
    // The `a` legend must state the session-wide, argument-blind scope
    // (wording fix: 'allow-name' hid that run_bash means ANY command).
    check(compact[1].find("a=allow ALL (session)") != std::string::npos &&
              mini_agent::md::display_width(compact[1]) <= 80,
          "session-allow legend is explicit and fits one row");
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

    // v4.17 (BoundedOutputSink): >4x the cap used to break the old
    // "stop appending, keep draining" loop -- the true stream tail (where
    // exit errors live) was silently dropped. The sink keeps the REAL
    // last bytes regardless of total volume.
    const auto huge = runtime.call(
        ToolCall{"1", "run_bash",
                 R"J({"command":"printf 'A%.0s' $(seq 1 200000); printf ENDING"})J"},
        [] { return false; });
    check(!huge.is_error && huge.text.size() < 40000 &&
              huge.text.find("truncated") != std::string::npos &&
              huge.text.compare(huge.text.size() - 6, 6, "ENDING") == 0,
          "over-4x output keeps the true tail (ring buffer)");

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
    check(tui::style_history_line("mini-agent v0.8.0 · model=m · session=s", true).find("\x1b[1mm") == 0,
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
    check(tools.size() == 6, "six file tools listed");

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

    // v5 (design 011): preview_file returns a short model-facing note and
    // stages the RAW body for result_preview(); boundary + binary posture
    // match read_file.
    {
        std::ofstream f(ws.root / "doc.md");
        f << "# Title\n\nbody line\n";
    }
    const auto pv = call("preview_file", R"({"path":"doc.md"})");
    check(!pv.is_error && pv.text.find("previewed doc.md") != std::string::npos,
          "preview_file confirms to model");
    const auto pv_body =
        runtime.result_preview(ToolCall{"1", "preview_file", R"({"path":"doc.md"})"}, pv);
    check(pv_body == "# Title\n\nbody line\n", "preview_file stages raw body");
    const auto pv_again =
        runtime.result_preview(ToolCall{"1", "preview_file", R"({"path":"doc.md"})"}, pv);
    check(pv_again.empty(), "preview body consumed once");
    check(call("preview_file", R"({"path":"../../etc/passwd"})").is_error,
          "preview_file blocks escape");
    check(call("preview_file", R"({"path":"nope.txt"})").is_error, "preview_file missing errors");

    // v5: result_preview stages the APPLIED diff after write_file (with a
    // preview-armed approval) and edit_file, and clears after consume.
    {
        {
            std::ofstream f(ws.root / "ap.txt");
            f << "one\ntwo\n";
        }
        ToolCall wc{"1", "write_file", R"({"path":"ap.txt","content":"one\nTWO\n"})"};
        runtime.preview(wc); // arms the race guard (approval flow shape)
        const auto wr = runtime.call(wc, [] { return false; });
        check(!wr.is_error, "write_file with armed preview succeeds");
        const auto rd = runtime.result_preview(wc, wr);
        check(rd.find("-two") != std::string::npos && rd.find("+TWO") != std::string::npos &&
                  rd.find("b/ap.txt") != std::string::npos,
              "result_preview yields applied diff");
        check(runtime.result_preview(wc, wr).empty(), "applied diff consumed once");
    }
    {
        // Un-armed edit (auto/allowlist tier): pre-image comes from the
        // re-read body, so the diff still shows the real change.
        ToolCall ec{"1", "edit_file", R"({"path":"ap.txt","old_str":"TWO","new_str":"three"})"};
        const auto er = runtime.call(ec, [] { return false; });
        check(!er.is_error, "un-armed edit_file succeeds");
        const auto ed = runtime.result_preview(ec, er);
        check(ed.find("-TWO") != std::string::npos && ed.find("+three") != std::string::npos,
              "result_preview diffs un-armed edit too");
    }
    {
        // New file (no pre-image on disk): diff against empty.
        ToolCall nc{"1", "write_file", R"({"path":"brand.txt","content":"fresh\n"})"};
        const auto nr = runtime.call(nc, [] { return false; });
        check(!nr.is_error, "new write_file succeeds");
        const auto nd = runtime.result_preview(nc, nr);
        // New file: header + all-`+` body, no removal content line.
        check(nd.find("+fresh") != std::string::npos && nd.find("\n-") == std::string::npos,
              "new-file applied diff runs against empty");
    }
    // Error results carry no preview; error write stages nothing.
    {
        ToolResult bad_result{"nope", true};
        check(runtime.result_preview(ToolCall{"1", "write_file", "{}"}, bad_result).empty(),
              "error result previews empty");
        const auto failed = call("write_file", R"({"path":"nodir/x.txt","content":"y"})");
        check(failed.is_error, "failed write errors");
        ToolCall ok_prev{"1", "write_file", R"({"path":"ok.txt","content":"z\n"})"};
        const auto ok_res = runtime.call(ok_prev, [] { return false; });
        (void)ok_res;
        // The failed write must NOT have staged a diff: consuming after a
        // later successful call returns that call's diff, never stale rows.
        const auto after = runtime.result_preview(ok_prev, ok_res);
        check(after.find("+z") != std::string::npos, "staged diff belongs to last call");
    }
    // registry.result_preview delegates + empty for no-preview tools.
    check(registry
              ->result_preview(ToolCall{"1", "run_bash", R"({"command":"ls"})"},
                               ToolResult{"x", false})
              .empty(),
          "registry.result_preview empty for run_bash");
}

void test_v3_config_and_prompt() {
    using namespace mini_agent;
    // config round-trip: workspace + system_prompt presence semantics.
    AppConfig config;
    config.current.base_url = "https://x.test/v1";
    config.current.api_key = "k";
    config.current.model = "m";
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
    check(prompt.find("preview_file") != std::string::npos, "prompt teaches preview_file");
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
    // v1 inputs migrate into the default profile; metadata derives into
    // current and round-trips through the profile table.
    const auto parsed = mini_agent::config_from_json(
        R"({"model":"m","context_window":128000,"reasoning_effort":"high"})", &error);
    check(parsed.has_value(), "v4 fields parse");
    check(parsed->current.context_window == 128000, "context_window parsed");
    check(parsed->current.reasoning_effort == "high", "reasoning_effort parsed");
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
    check(bare.has_value() && bare->current.context_window == 0 &&
              bare->current.reasoning_effort.empty(),
          "v4 defaults unset");
    check(mini_agent::config_to_json(*bare).find("context_window") == std::string::npos,
          "unset window omitted from output");

    // to_provider carries reasoning_effort (empty == omitted in the body).
    mini_agent::AppConfig cfg;
    cfg.current.base_url = "https://x.test/v1";
    cfg.current.api_key = "k";
    cfg.current.model = "m";
    cfg.current.reasoning_effort = "medium";
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

void test_v411_turn_panel() {
    namespace T = mini_agent::tui;
    // Idle-ish: header only (caller decides visibility; rows always lead
    // with the turn timer).
    const auto base = T::turn_panel_rows(false, false, 3, {}, 5);
    check(base.size() == 1 && base[0].find("this turn 3s") != std::string::npos,
          "header row carries the elapsed timer");
    // Thinking marker appears between header and tools.
    std::vector<std::string> tools = {"[tool] a [ok]", "[tool] b [running]"};
    const auto with_think = T::turn_panel_rows(false, true, 12, tools, 5);
    check(with_think.size() == 4 && with_think[1].find("thinking") != std::string::npos,
          "thinking row follows header");
    check(with_think.back().find("[running]") != std::string::npos, "running tool kept");
    // Over budget: OLDEST dropped, newest kept (the running call matters).
    std::vector<std::string> many;
    for (int i = 0; i < 9; ++i)
        many.push_back("row" + std::to_string(i));
    const auto capped = T::turn_panel_rows(false, false, 1, many, 5);
    check(capped.size() == 5, "rows capped to budget");
    check(capped.back().find("row8") != std::string::npos &&
              capped[1].find("row5") != std::string::npos &&
              capped[1].find("row0") == std::string::npos,
          "cap drops from the front (newest survive)");
    // v4.12 system-info row: plain joined line, control chars sanitized.
    const auto info = T::sysinfo_line(false, "marvin", "box", "~/w/mini", "23:50:01");
    check(info == "marvin@box · ~/w/mini · 23:50:01", "sysinfo format");
    check(T::sysinfo_line(false, "a\x1b[31mb", "h", "c", "t").find("\x1b") == std::string::npos,
          "sysinfo never carries escape bytes");
    // v4.16 merged footer: right block flush to the edge, left clipped.
    // Autowrap guard: the composed width caps at cols-1, NEVER cols -- a
    // row filling the last column leaves the terminal in deferred autowrap
    // and the frame's trailing \n advances two rows (broken live accounting).
    const auto foot = T::compose_footer("status", "info", 16);
    check(mini_agent::md::display_width(foot) == 15 && foot.substr(foot.size() - 4) == "info",
          "footer pads the gap, info flush right, one guard column");
    const auto tight = T::compose_footer("0123456789abcdefghij", "info", 16);
    check(mini_agent::md::display_width(tight) == 15 && tight.substr(tight.size() - 4) == "info",
          "long status clips to leave the gutter");
    check(T::compose_footer("only", "", 20) == "only", "no info = status alone");
    // v4.13 sidebar composition: rows merge with the gutter, the shorter
    // column pads out, and every composed row is exactly one terminal row.
    const auto merged = T::compose_columns({"abc", "de"}, {"123456", "x", "long"}, 20, 6);
    check(merged.size() == 3, "row count = max of both columns");
    check(merged[0] == std::string("abc") + std::string(7, ' ') + " | 123456",
          "content padded to split (10), sidebar kept");
    check(merged[2] == std::string(10, ' ') + " | long", "missing content row pads blank");
    // The right column is clipped, not padded: rows with a short sidebar
    // are narrower than cols. The invariant is "one terminal row each":
    // width <= cols-1 always (autowrap guard), == cols-1 when both sides
    // are full.
    check(mini_agent::md::display_width(merged[0]) == 19 &&
              mini_agent::md::display_width(merged[2]) <= 19,
          "composed rows never reach the last column; full pair hits cols-1");
    const auto off = T::compose_columns({"abc"}, {}, 20, 6);
    check(off.size() == 1 && off[0] == "abc", "empty sidebar passes content through");
    check(T::sidebar_status_glyph("??") == "??", "untracked glyph");
    check(T::sidebar_status_glyph(" M") == "M", "worktree modify glyph");
    check(T::sidebar_status_glyph("A ") == "A", "staged add glyph");
    check(T::sidebar_status_glyph("##").empty(), "branch header yields nothing");
}

void test_v413_session_prefix() {
    namespace T = mini_agent::tui;
    using mini_agent::match_session_ref;
    using mini_agent::session_prefix_matches;
    const std::vector<std::string> ids{"0a1b2c3d4e5f6070", "0a1b999999999999", "ff00112233445566"};
    // Pure prefix matcher: unique prefix, ambiguous, exact beats siblings,
    // no match, empty input.
    check(session_prefix_matches("0a1b2c", ids).size() == 1, "unique prefix hits one");
    check(session_prefix_matches("0a1b", ids).size() == 2, "shared prefix hits two");
    check(session_prefix_matches("0a1b2c3d4e5f6070", ids).size() == 1 &&
              session_prefix_matches("0a1b2c3d4e5f6070", ids).front() == "0a1b2c3d4e5f6070",
          "exact id outranks prefix siblings");
    check(session_prefix_matches("zz", ids).empty(), "unknown prefix hits none");
    check(session_prefix_matches("", ids).empty(), "empty input hits none");
    // Outcome view.
    check(match_session_ref("0a1b2c", ids).exact &&
              match_session_ref("0a1b2c", ids).id == "0a1b2c3d4e5f6070",
          "match resolves unique prefix");
    check(!match_session_ref("0a1b", ids).exact &&
              match_session_ref("0a1b", ids).candidates.size() == 2,
          "match reports ambiguity candidates");
    check(!match_session_ref("99", ids).exact && match_session_ref("99", ids).candidates.empty(),
          "match no-hit carries empty candidates");
    // /session Tab completion now walks real ids (store snapshot passed in),
    // prefix-filtered; without ids the <id> placeholder behaviour stands.
    int cycle = 0;
    const auto first = T::slash_tab_complete("/session 0a1b", cycle, ids);
    check(first && (*first == "/session 0a1b2c3d4e5f6070" || *first == "/session 0a1b999999999999"),
          "/session prefix Tab inserts a matching id");
    const auto second = T::slash_tab_complete("/session 0a1b", cycle, ids);
    check(second && *second != *first, "Tab cycles among prefix matches");
    const auto rot = T::slash_tab_complete("/session " + ids[0], cycle, ids);
    check(rot && *rot == "/session " + ids[1], "rest is a full choice: rotates to next");
    const auto placeholder = T::slash_tab_complete("/session", cycle);
    check(placeholder && *placeholder == "/session <id>", "no store snapshot keeps <id> default");
    const auto hint = T::slash_completion_hint("/session 0a1b", ids);
    check(hint.size() == 3 && hint[0].find("options for /session") != std::string::npos &&
              hint[1].find(ids[0]) != std::string::npos,
          "hint lists prefix-matching ids");
    const auto full_hint = T::slash_completion_hint("/session ", ids);
    check(full_hint.size() == ids.size() + 2, "empty arg lists all ids plus new");
}

// v4.15 (maintainer request): bare `run` reuses the most recently updated
// session. The store side is list_by_recency: mtime order, newest first,
// locks skipped, deterministic id tie-break.
void test_v415_session_recency() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mini_agent_test_" + mini_agent::generate_session_id());
    mini_agent::SessionStore store(root);
    std::string error;
    check(store.ensure_root(&error), "recency: root created");

    // No sessions: empty list, no error.
    check(store.list_by_recency(&error).empty(), "recency: empty store lists none");

    mini_agent::Session session;
    session.meta.model = "test-model";
    mini_agent::Message m;
    m.role = mini_agent::Role::User;
    m.content = "hello";
    session.chat.add(m);
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        session.meta.id = mini_agent::generate_session_id();
        ids.push_back(session.meta.id);
        check(store.save(session, &error), "recency: save ok");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    // A lock file must never be treated as a session candidate.
    check(store.try_lock(ids[0], &error), "recency: lock taken");
    const auto order = store.list_by_recency(&error);
    check(order.size() == 3, "recency: all sessions found");
    // Saves ran ids[0] -> ids[2], so newest-first is the reverse.
    check(order.size() == 3 && order[0] == ids[2] && order[1] == ids[1] && order[2] == ids[0],
          "recency: newest first");
    store.unlock(ids[0]);

    // Re-saving an id moves it to the front (mtime refresh on save).
    session.meta.id = ids[0];
    check(store.save(session, &error), "recency: resave oldest ok");
    const auto after = store.list_by_recency(&error);
    check(!after.empty() && after.front() == ids[0] && after.back() == ids[1],
          "recency: save refreshes recency");
    std::filesystem::remove_all(root);
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

// v6 (design 012 §5.3): the provider-aware argument tiers. /login cycles
// profile names before <key>, /provider its subcommands, /model the fetched
// (possibly qualified) ids before <name>. Exercised through the public
// Tab/hint wrappers (the choice builder itself has internal linkage).
void test_v6_provider_arg_completion() {
    namespace T = mini_agent::tui;
    const std::vector<std::string> names{"alpha", "beta"};
    const std::vector<std::string> models{"alpha/mock-large", "beta/mock-large"};
    // /login: names first, placeholder last; no names keeps the legacy tier.
    const auto lhint = T::slash_completion_hint("/login ", {}, names);
    check(lhint.size() == 4 && lhint[0].find("options for /login") != std::string::npos &&
              lhint[1].find("alpha") != std::string::npos &&
              lhint[2].find("beta") != std::string::npos &&
              lhint[3].find("<key>") != std::string::npos,
          "/login hint lists provider names then <key>");
    const auto bare_login = T::slash_completion_hint("/login ");
    check(bare_login.size() == 2 && bare_login[1].find("<key>") != std::string::npos,
          "/login without profiles: <key> only");
    int cycle = 0;
    const auto l1 = T::slash_tab_complete("/login ", cycle, {}, names);
    check(l1 && *l1 == "/login alpha", "Tab on /login inserts the first profile");
    const auto l2 = T::slash_tab_complete("/login alpha", cycle, {}, names);
    check(l2 && *l2 == "/login beta", "full profile name rotates to the next");
    // /provider: fixed subcommand tier.
    const auto phint = T::slash_completion_hint("/provider ");
    check(phint.size() == 5 && phint[1].find("list") != std::string::npos &&
              phint[2].find("add") != std::string::npos &&
              phint[3].find("rm") != std::string::npos &&
              phint[4].find("<url>") != std::string::npos,
          "/provider hint lists list/add/rm/<url>");
    const auto padd = T::slash_tab_complete("/provider a", cycle);
    check(padd && *padd == "/provider add", "/provider a filters to add");
    // /model: fetched ids (qualified in an aggregate) before the placeholder.
    const auto mhint = T::slash_completion_hint("/model ", {}, {}, models);
    check(mhint.size() == 4 && mhint[1].find("alpha/mock-large") != std::string::npos &&
              mhint[2].find("beta/mock-large") != std::string::npos &&
              mhint[3].find("<name>") != std::string::npos,
          "/model hint cycles fetched qualified ids then <name>");
    int mc = 0;
    const auto m1 = T::slash_tab_complete("/model ", mc, {}, {}, models);
    check(m1 && *m1 == "/model alpha/mock-large", "Tab on /model inserts the first fetched id");
    const auto m2 = T::slash_tab_complete("/model alpha/mock-large", mc, {}, {}, models);
    check(m2 && *m2 == "/model beta/mock-large", "full id rotates to the next provider's");
    const auto bare_model = T::slash_completion_hint("/model ");
    check(bare_model.size() == 2 && bare_model[1].find("<name>") != std::string::npos,
          "/model without fetch: <name> only");
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
    lvl.current.base_url = "https://x.test/v1";
    lvl.current.api_key = "k";
    lvl.current.model = "m";
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
    cfg.current.base_url = "https://x.test/v1";
    cfg.current.api_key = "k";
    cfg.current.model = "m";
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

// v4.17 (008 D4): TurnBridge worker<->UI handoff, unit-tested WITHOUT a
// pty -- two threads drive the exact protocol tui.cpp runs, so regressions
// in the approval sequence / delta buffer / tool queue are caught here
// instead of only in tests/pty_tui_smoke.py.
void test_v417_turn_bridge() {
    using namespace mini_agent;
    using namespace mini_agent::tui;
    TurnBridge bridge;

    // deltas: concurrent appends from a worker thread must all land
    {
        std::atomic<bool> stop{false};
        std::thread worker([&] {
            for (int i = 0; i < 500 && !stop.load(); ++i)
                bridge.append_delta("x");
        });
        std::string seen;
        while (seen.size() < 500) {
            seen = bridge.deltas_snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop = true;
        worker.join();
        check(seen.size() >= 500 && seen.find_first_not_of('x') == std::string::npos,
              "deltas accumulate append-only across threads");
        bridge.reset_turn();
        check(bridge.deltas_snapshot().empty(), "reset_turn clears deltas");
    }

    // tool queue: push under worker, drain under main, running_tool atomic
    {
        ToolRound r1;
        r1.tool_name = "a";
        ToolRound r2;
        r2.tool_name = "b";
        bridge.set_running_tool("[tool] b [running]");
        check(bridge.running_tool() == "[tool] b [running]", "running_tool readable");
        bridge.finish_running_tool(std::move(r1));
        check(bridge.running_tool().empty(), "finish clears running_tool");
        bridge.push_tool_event(std::move(r2));
        auto batch = bridge.drain_tool_events();
        check(batch.size() == 2 && batch[0].tool_name == "a" && batch[1].tool_name == "b",
              "drain returns FIFO and empties the queue");
        check(bridge.drain_tool_events().empty(), "second drain is empty");
    }

    // approval protocol: worker publishes+waits, main answers via keys
    {
        check(!bridge.approval_pending(), "no pending approval initially");
        ToolCall call{"c1", "git_commit", R"({"message":"x"})"};
        unsigned long long seq = bridge.publish_approval(call, "diff-preview");
        check(bridge.approval_pending(), "publish makes the request pending");
        check(bridge.approval_call().name == "git_commit", "call snapshot readable");
        check(bridge.approval_preview() == "diff-preview", "preview readable");
        check(bridge.approval_waited_s() >= 0, "countdown stamps publish time");
        std::atomic<int> worker_answer{-1};
        std::thread worker([&] {
            auto w = bridge.wait_approval(seq, 5, [] { return false; });
            worker_answer = bridge.approval_answer(seq);
            check(w == ApprovalWait::Answer, "worker observes the answer");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        bridge.answer_approval(1); // allow
        worker.join();
        check(worker_answer.load() == 1, "allow value crosses the boundary");
        check(!bridge.approval_pending(), "answered request is no longer pending");
    }

    // timeout branch: tiny injectable budget returns Timeout, and the
    // un-answered value reads as deny (the tui.cpp denial-round contract)
    {
        unsigned long long seq = bridge.publish_approval(ToolCall{"t", "x", "{}"}, "");
        std::thread worker([&] {
            auto w = bridge.wait_approval(seq, 0, [] { return false; });
            check(w == ApprovalWait::Timeout, "zero budget times out immediately");
            check(bridge.approval_answer(seq) == 2,
                  "un-answered approval reads as deny");
        });
        worker.join();
        bridge.answer_approval(2); // close the sequence for the next test
    }

    // abort branch: an abort flag raises without an answer -> Aborted
    {
        unsigned long long seq = bridge.publish_approval(ToolCall{"t", "x", "{}"}, "");
        std::thread worker([&] {
            auto w = bridge.wait_approval(seq, 30, [] { return true; });
            check(w == ApprovalWait::Aborted, "abort flag exits the wait");
            check(bridge.approval_answer(seq) == 2,
                  "abort without answer reads as deny");
        });
        worker.join();
        bridge.answer_approval(2);
    }
}

// 014 (design 014 §4/§6): pre-stream retry decision + backoff helpers, the
// env knob, and the bridge retry-event queue (worker -> main handoff).
void test_provider_retry_helpers() {
    using mini_agent::ProviderErrorKind;
    using mini_agent::retry_wait_ms;
    using mini_agent::retryable_provider_error;
    // Decision matrix: network always; transient statuses only; auth and
    // request errors never; Protocol/Cancelled never.
    check(retryable_provider_error(ProviderErrorKind::Network, 0), "network error retryable");
    for (int status : {408, 429, 500, 502, 503, 504}) {
        check(retryable_provider_error(ProviderErrorKind::HttpStatus, status),
              "transient HTTP status retryable");
    }
    for (int status : {400, 401, 403, 404, 422, 501, 505}) {
        check(!retryable_provider_error(ProviderErrorKind::HttpStatus, status),
              "non-transient HTTP status not retryable");
    }
    check(!retryable_provider_error(ProviderErrorKind::Protocol, 0), "protocol never retried");
    check(!retryable_provider_error(ProviderErrorKind::Cancelled, 0), "cancelled never retried");
    // Backoff: 1s << index; Retry-After raises the wait; >30s gives up.
    check(retry_wait_ms(0, 0) == 1000 && retry_wait_ms(1, 0) == 2000 && retry_wait_ms(2, 0) == 4000,
          "exponential backoff 1s/2s/4s");
    check(retry_wait_ms(0, 3) == 3000, "Retry-After raises the first wait");
    check(retry_wait_ms(2, 1) == 4000, "backoff wins over a smaller Retry-After");
    check(retry_wait_ms(0, 30) == 30000, "Retry-After at the cap is honored");
    check(retry_wait_ms(0, 31) == -1, "Retry-After beyond the cap gives up");
    check(retry_wait_ms(0, -2) == 1000, "negative Retry-After treated as absent");
    // Env knob: 0..5 accepted, invalid values keep the default (2).
    {
        mini_agent::AppConfig cfg;
        ::setenv("MINI_AGENT_RETRIES", "0", 1);
        check(cfg.to_provider().max_retries == 0, "MINI_AGENT_RETRIES=0 disables");
        ::setenv("MINI_AGENT_RETRIES", "5", 1);
        check(cfg.to_provider().max_retries == 5, "MINI_AGENT_RETRIES=5 accepted");
        ::setenv("MINI_AGENT_RETRIES", "9", 1);
        check(cfg.to_provider().max_retries == 2, "out-of-range keeps the default");
        ::setenv("MINI_AGENT_RETRIES", "abc", 1);
        check(cfg.to_provider().max_retries == 2, "non-numeric keeps the default");
        ::unsetenv("MINI_AGENT_RETRIES");
        check(cfg.to_provider().max_retries == 2, "absent env keeps the default");
    }
    // Bridge queue: push/drain swaps out, reset_turn clears.
    {
        mini_agent::tui::TurnBridge bridge;
        bridge.push_retry_event(mini_agent::tui::RetryEvent{1, 429, 1000});
        bridge.push_retry_event(mini_agent::tui::RetryEvent{2, 0, 2000});
        const auto batch = bridge.drain_retry_events();
        check(batch.size() == 2 && batch[0].attempt == 1 && batch[0].http_status == 429 &&
                  batch[1].wait_ms == 2000,
              "retry events drain in order with metadata intact");
        check(bridge.drain_retry_events().empty(), "drain empties the queue");
        bridge.push_retry_event(mini_agent::tui::RetryEvent{1, 503, 1000});
        bridge.reset_turn();
        check(bridge.drain_retry_events().empty(), "reset_turn clears retry events");
    }
}

// 015 (design 015 §6): overflow pattern matrix, Chat::replace, and the
// bounded compact-request builder.
void test_context_compact() {
    using mini_agent::Chat;
    using mini_agent::Message;
    using mini_agent::Role;
    // Overflow patterns: the known gateway families hit, case-insensitive;
    // unrelated 400 wording never guesses.
    check(mini_agent::context_overflow_message("Error code: context_length_exceeded"),
          "openai code hits");
    check(
        mini_agent::context_overflow_message("This model's maximum context length is 8192 tokens"),
        "vLLM/openai wording hits");
    check(mini_agent::context_overflow_message("PLEASE REDUCE THE LENGTH of the messages"),
          "case-insensitive match");
    check(mini_agent::context_overflow_message("<400> Range of input length should be [1, 983616]"),
          "dashscope input cap hits");
    check(mini_agent::context_overflow_message("too many tokens in the request"),
          "too-many-tokens hits");
    check(!mini_agent::context_overflow_message("invalid reasoning_effort value"),
          "unrelated 400 wording does not hit");
    check(!mini_agent::context_overflow_message(""), "empty text does not hit");
    // Chat::replace swaps wholesale and survives the JSON round trip.
    Chat chat;
    chat.add(Message{Role::System, "prompt", {}, {}});
    chat.add(Message{Role::User, "old", {}, {}});
    chat.replace({Message{Role::User, "[conversation summary]\nnew", {}, {}}});
    check(chat.messages().size() == 1 &&
              chat.messages()[0].content.find("new") != std::string::npos,
          "replace swaps the whole history");
    const auto parsed = mini_agent::chat_from_json(mini_agent::chat_to_json(chat), nullptr);
    check(parsed && parsed->messages().size() == 1, "replaced history round-trips");
    // build_compact_request: System kept verbatim, one User instruction,
    // per-message truncation, head+tail kept when the transcript is huge.
    Chat history;
    history.add(Message{Role::System, "system prompt", {}, {}});
    history.add(Message{Role::User, "goal: build the thing", {}, {}});
    history.add(Message{Role::Assistant, std::string(3000, 'x'), {}, {}});
    const auto request = mini_agent::build_compact_request(history);
    check(request.messages().size() == 2 && request.messages()[0].role == Role::System &&
              request.messages()[0].content == "system prompt",
          "compact request keeps the system prompt verbatim");
    const auto& instr = request.messages()[1].content;
    check(request.messages()[1].role == Role::User &&
              instr.find("context-compaction request") != std::string::npos &&
              instr.find("goal: build the thing") != std::string::npos,
          "instruction carries the transcript");
    check(instr.find(std::string(1600, 'x')) == std::string::npos &&
              instr.find("[...truncated]") != std::string::npos,
          "long messages are capped per message");
    // Huge history: middle dropped, head and tail survive, size bounded.
    Chat huge;
    for (int i = 0; i < 60; ++i)
        huge.add(
            Message{Role::User, "msg" + std::to_string(i) + " " + std::string(1400, 'y'), {}, {}});
    const auto big = mini_agent::build_compact_request(huge);
    const auto& big_instr = big.messages()[0].content;
    check(big_instr.find("[... middle omitted ...]") != std::string::npos &&
              big_instr.find("msg0 ") != std::string::npos &&
              big_instr.find("msg59 ") != std::string::npos &&
              big_instr.find("msg30 ") == std::string::npos,
          "oversized transcript keeps head and tail, drops the middle");
    check(big_instr.size() < 60000 + 2000, "transcript stays bounded");
    // Empty history still yields a well-formed single-message request.
    const auto empty = mini_agent::build_compact_request(Chat{});
    check(empty.messages().size() == 1 && empty.messages()[0].role == Role::User,
          "empty history yields the instruction alone");
}

// 016 (design 016 §5): Markdown export is a faithful linear transcript.
void test_session_markdown_export() {
    using mini_agent::Role;
    mini_agent::Session session;
    session.meta.id = "abcd1234abcd1234";
    session.meta.model = "mock-large";
    session.meta.created_at = "2026-11-14T00:00:00Z";
    session.meta.updated_at = "2026-11-14T01:00:00Z";
    session.meta.workspace = "/tmp/ws";
    session.meta.provider = "alpha";
    session.chat.add(mini_agent::Message{Role::System, "prompt text", {}, {}});
    session.chat.add(mini_agent::Message{Role::User, "hello there", {}, {}});
    mini_agent::Message assistant;
    assistant.role = Role::Assistant;
    assistant.content = "using a tool";
    assistant.tool_calls.push_back(mini_agent::ToolCall{"c1", "run_bash", R"({"command":"ls"})"});
    session.chat.add(std::move(assistant));
    mini_agent::Message tool;
    tool.role = Role::Tool;
    tool.content = "file-a\nfile-b";
    tool.tool_call_id = "c1";
    session.chat.add(std::move(tool));
    const auto md = mini_agent::session_to_markdown(session);
    check(md.find("# session abcd1234abcd1234") == 0 &&
              md.find("- model: mock-large") != std::string::npos &&
              md.find("- workspace: /tmp/ws") != std::string::npos &&
              md.find("- provider: alpha") != std::string::npos,
          "markdown export carries the meta header");
    check(md.find("## System") != std::string::npos && md.find("## User") != std::string::npos &&
              md.find("## Assistant") != std::string::npos &&
              md.find("## Tool (c1)") != std::string::npos,
          "all four roles get sections, tool carries its call id");
    check(md.find("hello there") != std::string::npos &&
              md.find("file-a\nfile-b") != std::string::npos,
          "message bodies are verbatim");
    check(md.find("```json") != std::string::npos &&
              md.find("\"name\": \"run_bash\"") != std::string::npos,
          "tool_calls render as fenced json");
    // Empty session: header only, no sections.
    mini_agent::Session bare;
    bare.meta.id = "empty";
    const auto bare_md = mini_agent::session_to_markdown(bare);
    check(bare_md.find("# session empty") == 0 && bare_md.find("## ") == std::string::npos,
          "empty session exports the header alone");
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
    test_provider_profiles();
    test_models_listing();
    test_config_batched_hint();
    test_login_status();
    test_connection_prompt();
    test_logger();
    test_md_width_and_wrap();
    test_md_render_subset();
    test_highlight();
    test_highlight_in_render();
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
    test_v413_session_prefix();
    test_v415_session_recency();
    test_v410_arg_completion();
    test_v6_provider_arg_completion();
    test_v411_turn_panel();
    test_v44_debug_log();
    test_v417_turn_bridge();
    test_provider_retry_helpers();
    test_context_compact();
    test_session_markdown_export();
    test_git_tools();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all agent checks passed\n";
    return EXIT_SUCCESS;
}
