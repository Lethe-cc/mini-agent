#include "mini_agent/provider.hpp"

#include <curl/curl.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace mini_agent {
namespace {

// Incremental SSE line splitter. Feeds complete `data:` payloads out of a
// byte stream that may split lines anywhere.
class SseLines {
  public:
    void feed(std::string_view chunk) {
        buffer_.append(chunk);
        std::string::size_type nl;
        while ((nl = buffer_.find('\n')) != std::string::npos) {
            auto line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (starts_with(line, "data:")) {
                auto payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ')
                    payload.erase(0, 1);
                on_data_(std::move(payload));
            }
        }
    }

  private:
    static bool starts_with(const std::string& text, const char* prefix) {
        return text.compare(0, std::strlen(prefix), prefix) == 0;
    }

  public:
    std::function<void(std::string&&)> on_data_;

  private:
    std::string buffer_;
};

struct CurlWriteCtx {
    SseLines* lines;
    const ShouldAbort* should_abort;
};

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<CurlWriteCtx*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (ctx->should_abort && (*ctx->should_abort)()) {
        return 0; // abort transfer
    }
    ctx->lines->feed(std::string_view(ptr, bytes));
    return bytes;
}

// Sink for non-streaming GETs (e.g. /models): accumulate the whole body.
size_t curl_accumulate(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

namespace {
// v4.4 (design 010 §15): raw chat-traffic debug dump. Opt-in via
// ProviderConfig::debug_log_path; append-only, 0600, one file shared by
// all providers in the process. Only the conversation path calls this
// (complete_chat): GET /models and the context probe never dump, so
// auxiliary traffic stays out of the file by construction. Bodies here
// are model I/O content -- the api_key lives in the Authorization
// header only and is never part of a dumped payload.
void debug_dump(const std::string& path, std::string_view tag, std::string_view payload) {
    if (path.empty())
        return;
    static std::mutex dump_mutex;
    std::lock_guard<std::mutex> lock(dump_mutex);
    const bool fresh = !std::filesystem::exists(path);
    std::ofstream file(path, std::ios::app);
    if (!file)
        return; // debug is best-effort; never breaks the request path
    file << "=== " << tag << " "
         << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()
         << "ms ===\n"
         << payload << '\n';
    file.close();
    if (fresh)
        ::chmod(path.c_str(), 0600);
}

// v4.6: level-2 dump for auxiliary traffic (GET /models, context probe).
// Payloads are truncated to a bounded size -- the probe stage-2 request
// alone is ~2.4 MB of filler -- so the file stays readable while the
// error-message part (what debugging actually needs) survives. Chat
// dumps keep going through debug_dump untruncated.
void debug_dump_aux(const std::string& path, std::string_view tag, std::string_view payload) {
    constexpr std::size_t kAuxCap = 4096;
    if (path.empty())
        return;
    if (payload.size() <= kAuxCap) {
        debug_dump(path, tag, payload);
        return;
    }
    std::string trimmed(payload.substr(0, kAuxCap));
    trimmed += "...[truncated " + std::to_string(payload.size() - kAuxCap) + " bytes]";
    debug_dump(path, tag, trimmed);
}
} // namespace

std::string request_body(const Chat& chat, const std::string& model, const std::string& tools_json,
                         const std::string& reasoning_effort = {}, bool include_usage = false) {
    nlohmann::json root;
    root["model"] = model;
    root["stream"] = true;
    // v4: ask the gateway to append a final usage chunk to the SSE stream.
    // Off by default so preview_chat_request_body (and thus the v1
    // byte-identical regression hook) keeps the historical shape; the real
    // completion path turns it on.
    if (include_usage) {
        root["stream_options"] = {{"include_usage", true}};
    }
    // v4: verbatim passthrough when configured; omitted otherwise (server
    // default). Empty == byte-identical pre-v4 request.
    if (!reasoning_effort.empty()) {
        root["reasoning_effort"] = reasoning_effort;
    }
    root["messages"] = nlohmann::json::parse(chat_to_json(chat));
    // Assistant messages carrying tool_calls must send the wire shape
    // {id, type:"function", function:{name, arguments}} (design 008 5.2:
    // storage stays vendor-neutral, the Provider maps here).
    for (auto& message : root["messages"]) {
        if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
            for (auto& call : message["tool_calls"]) {
                std::string id = call.value("id", "");
                std::string name = call.value("name", "");
                std::string args = call.value("arguments", "");
                call = {{"id", id},
                        {"type", "function"},
                        {"function", {{"name", name}, {"arguments", args}}}};
            }
        }
    }
    if (!tools_json.empty() && tools_json != "[]") {
        auto tools = nlohmann::json::parse(tools_json, nullptr, false);
        if (!tools.is_discarded() && tools.is_array() && !tools.empty()) {
            root["tools"] = std::move(tools);
        }
    }
    return root.dump();
}

// Accumulates streamed delta.tool_calls fragments keyed by their `index`
// (OpenAI split rule: id/name usually arrive once, arguments stream in
// pieces). Lenient toward gateway variants (008 section 9 R2 -- qwen/
// DashScope compat endpoints diverge): `call_id` as id alias, flattened
// name/arguments next to index (no "function" wrapper), arguments
// arriving as a parsed object instead of a string, and the legacy
// delta.function_call stream (feed_legacy). Also records a STRUCTURE
// FINGERPRINT -- field names and JSON types only, never values -- so an
// unrecognized shape reports what the stream actually looked like
// without leaking model output or user data (log red line, 007 section 6).
class ToolCallAccumulator {
  public:
    bool saw_fragments() const noexcept {
        return saw_fragments_;
    }

    std::string shape_summary() const {
        std::string out;
        std::size_t shown = 0;
        for (const auto& token : shape_) {
            if (shown++ >= 10) {
                out += "; ...";
                break;
            }
            if (!out.empty())
                out += "; ";
            out += token;
        }
        if (out.empty())
            out = "no fragments seen";
        return out;
    }

    void feed(const nlohmann::json& delta_tool_calls) {
        saw_fragments_ = saw_fragments_ || !delta_tool_calls.empty();
        for (const auto& frag : delta_tool_calls) {
            if (!frag.is_object())
                continue;
            note_shape("tool_calls", frag);
            std::size_t index = 0;
            if (auto it = frag.find("index"); it != frag.end() && it->is_number_integer()) {
                auto raw = it->get<int64_t>();
                if (raw < 0)
                    continue;
                index = static_cast<std::size_t>(raw);
            }
            if (slots_.size() <= index)
                slots_.resize(index + 1);
            auto& slot = slots_[index];
            // Non-empty-only assignment: qwen/DashScope compat gateways
            // commonly lead with EMPTY id/name placeholders and fill the
            // real values in later fragments (structure fingerprint from
            // live test 2026-11-04); an unconditional write would let the
            // empty later chunk wipe a resolved id/name.
            if (auto it = frag.find("id"); it != frag.end() && it->is_string()) {
                const auto v = it->get<std::string>();
                if (!v.empty())
                    slot.id = v;
            } else if (auto it = frag.find("call_id"); it != frag.end() && it->is_string()) {
                const auto v = it->get<std::string>();
                if (!v.empty())
                    slot.id = v; // dashscope-style alias
            }
            // OpenAI nests under "function"; flattened gateways keep the
            // fields directly on the fragment.
            const nlohmann::json* fields = &frag;
            const auto fn = frag.find("function");
            if (fn != frag.end() && fn->is_object())
                fields = &(*fn);
            if (auto name = fields->find("name"); name != fields->end() && name->is_string()) {
                const auto v = name->get<std::string>();
                if (!v.empty())
                    slot.name = v;
            }
            if (auto args = fields->find("arguments"); args != fields->end()) {
                if (args->is_string()) {
                    // placeholder empty args must not pre-seed the buffer
                    slot.arguments_json += args->get<std::string>();
                } else if (args->is_object() || args->is_array()) {
                    slot.arguments_json = args->dump(); // pre-parsed shape
                }
            }
        }
    }

    // Message-level complete call (some gateways deliver the final chunk
    // with choices[0].message.tool_calls instead of delta fragments).
    void feed_message(const nlohmann::json& tool_calls_array) {
        if (!tool_calls_array.is_array())
            return;
        for (const auto& call : tool_calls_array) {
            if (!call.is_object())
                continue;
            note_shape("message_tool_calls", call);
            std::size_t index = 0;
            if (auto it = call.find("index"); it != call.end() && it->is_number_integer()) {
                auto raw = it->get<int64_t>();
                if (raw >= 0)
                    index = static_cast<std::size_t>(raw);
            }
            if (slots_.size() <= index)
                slots_.resize(index + 1);
            auto& slot = slots_[index];
            if (auto it = call.find("id"); it != call.end() && it->is_string())
                slot.id = it->get<std::string>();
            const auto fn = call.find("function");
            if (fn != call.end() && fn->is_object()) {
                if (auto name = fn->find("name"); name != fn->end() && name->is_string())
                    slot.name = name->get<std::string>();
                if (auto args = fn->find("arguments"); args != fn->end()) {
                    if (args->is_string())
                        slot.arguments_json = args->get<std::string>();
                    else if (args->is_object() || args->is_array())
                        slot.arguments_json = args->dump();
                }
            }
        }
        saw_fragments_ = true;
    }

    // Legacy single-shot stream: delta.function_call = {name?, arguments
    // fragment?}; synthesizes an id so the loop can execute it.
    void feed_legacy(const nlohmann::json& call) {
        saw_fragments_ = true;
        note_shape("function_call", call);
        if (!call.is_object())
            return;
        if (slots_.empty())
            slots_.resize(1);
        auto& slot = slots_.front();
        if (slot.id.empty())
            slot.id = "call_function_1";
        if (auto name = call.find("name"); name != call.end() && name->is_string())
            slot.name = name->get<std::string>();
        if (auto args = call.find("arguments"); args != call.end()) {
            if (args->is_string())
                slot.arguments_json += args->get<std::string>();
            else if (args->is_object() || args->is_array())
                slot.arguments_json = args->dump();
        }
    }

    // Extracts completed calls: entries that carry both id and name.
    // Resolution diagnostics: per-slot id/name BYTE LENGTHS and argument
    // length only -- no values (log red line, 007 section 6).
    std::string resolution_summary() const {
        std::string out;
        for (std::size_t i = 0; i < slots_.size() && i < 4; ++i) {
            if (!out.empty())
                out += ",";
            out += std::to_string(slots_[i].id.size()) + "/" +
                   std::to_string(slots_[i].name.size()) + "/" +
                   std::to_string(slots_[i].arguments_json.size());
        }
        return out.empty() ? std::string("none") : out;
    }

    std::vector<ToolCall> finish() {
        std::vector<ToolCall> out;
        for (auto& slot : slots_) {
            if (!slot.id.empty() && !slot.name.empty())
                out.push_back(std::move(slot));
        }
        slots_.clear();
        return out;
    }

  private:
    void note_shape(const char* where, const nlohmann::json& node) {
        if (!node.is_object() || shape_.size() > 40)
            return;
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key(it.key());
            if (it->is_object()) {
                shape_.insert(std::string(where) + "." + key + "=obj");
                for (auto sub = it->begin(); sub != it->end(); ++sub)
                    shape_.insert(std::string(where) + "." + key + "." + std::string(sub.key()) +
                                  "=" + type_name(*sub));
            } else {
                shape_.insert(std::string(where) + "." + key + "=" + type_name(*it));
            }
        }
    }
    static const char* type_name(const nlohmann::json& v) {
        if (v.is_string())
            return "str";
        if (v.is_number_integer())
            return "int";
        if (v.is_number())
            return "num";
        if (v.is_boolean())
            return "bool";
        if (v.is_object())
            return "obj";
        if (v.is_array())
            return "arr";
        return "null";
    }

    std::vector<ToolCall> slots_;
    bool saw_fragments_ = false;
    std::set<std::string> shape_;
};

} // namespace

// Test hook (design 008 section 7 regression): exposes the internal request
// builder (anon-namespace) to agent_test so the no-tools path can be proven
// byte-identical after the v2 merge. Not part of any public interface.
std::string preview_chat_request_body(const Chat& chat, const std::string& model,
                                      const std::string& tools_json,
                                      const std::string& reasoning_effort, bool include_usage) {
    return request_body(chat, model, tools_json, reasoning_effort, include_usage);
}

std::string OpenAiProvider::complete(const Chat& chat,
                                     const std::function<void(std::string_view)>& on_delta,
                                     const ShouldAbort& should_abort) {
    return complete_chat(chat, std::string(), on_delta, should_abort).text;
}

CompletionResult Provider::complete_chat(const Chat& chat, const std::string&,
                                         const std::function<void(std::string_view)>& on_delta,
                                         const ShouldAbort& should_abort) {
    // v1 fallback for providers without a tool loop (design 008 section
    // 5.2): tools are advertised in the request but the base transport
    // returns text only.
    CompletionResult text_only; // v1 fallback: no tool loop, no usage
    text_only.text = complete(chat, on_delta, should_abort);
    return text_only;
}

CompletionResult
OpenAiProvider::complete_chat(const Chat& chat, const std::string& tools_json,
                              const std::function<void(std::string_view)>& on_delta,
                              const ShouldAbort& should_abort) {
    if (config_.base_url.empty() || config_.api_key.empty() || config_.model.empty()) {
        throw ProviderError(ProviderErrorKind::Protocol, "provider config incomplete");
    }
    CURL* curl = ::curl_easy_init();
    if (!curl) {
        throw ProviderError(ProviderErrorKind::Network, "curl init failed");
    }
    const std::string url = config_.base_url;
    const auto trimmed = url.find_last_not_of('/');
    std::string endpoint = trimmed == std::string::npos ? url : url.substr(0, trimmed + 1);
    endpoint += "/chat/completions";

    const std::string body = request_body(chat, config_.model, tools_json, config_.reasoning_effort,
                                          /*include_usage = */ true);
    const std::string auth = "Authorization: Bearer " + config_.api_key;
    const std::string type = "Content-Type: application/json";
    const std::string accept = "Accept: text/event-stream";
    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, auth.c_str());
    headers = ::curl_slist_append(headers, type.c_str());
    headers = ::curl_slist_append(headers, accept.c_str());

    std::string full;
    std::string http_error;
    std::string finish_reason;
    TokenUsage usage;          // v4: filled by the usage chunk when the gateway sends one
    ActualReasoning reasoning; // v4.2: evidence collected across all chunks
    // v4.4: chat traffic only (design 010 §15). One dump per round trip.
    debug_dump(config_.debug_log_path, "REQUEST", body);
    ToolCallAccumulator tool_calls;
    SseLines lines;
    lines.on_data_ = [&](std::string&& payload) {
        debug_dump(config_.debug_log_path, "SSE", payload);
        if (payload == "[DONE]")
            return;
        auto parsed = nlohmann::json::parse(payload, nullptr, false);
        if (parsed.is_discarded()) {
            throw ProviderError(ProviderErrorKind::Protocol, "malformed SSE JSON payload");
        }
        const auto top = parsed.find("error");
        if (top != parsed.end() && top->is_object()) {
            std::string msg = "provider error";
            if (auto m = top->find("message"); m != top->end() && m->is_string()) {
                msg = m->get<std::string>();
            }
            throw ProviderError(ProviderErrorKind::Protocol, msg);
        }
        const auto choices = parsed.find("choices");
        // v4.2: a `reasoning_effort` string echoes the effective level
        // back. Gateways place it at the payload top level (OpenRouter)
        // or per-choice (observed mock/compat shapes) -- check both.
        const auto note_effort = [&reasoning](const nlohmann::json& node) {
            if (auto re = node.find("reasoning_effort");
                re != node.end() && re->is_string() && !re->get_ref<const std::string&>().empty()) {
                reasoning.effort = re->get<std::string>();
                reasoning.seen = true;
            }
        };
        note_effort(parsed);
        if (choices != parsed.end() && choices->is_array() && !choices->empty())
            note_effort(choices->front());
        // v4: usage chunk (stream_options.include_usage) carries `usage`
        // with an empty/absent choices array -- read it before the choices
        // guard below returns early. Last-seen wins (gateways that emit
        // per-chunk usage end with the authoritative total).
        if (auto u = parsed.find("usage"); u != parsed.end() && u->is_object()) {
            const auto num = [&u](const char* key) -> long long {
                if (auto it = u->find(key); it != u->end() && it->is_number_integer())
                    return it->get<long long>();
                return 0;
            };
            usage.prompt_tokens = num("prompt_tokens");
            usage.completion_tokens = num("completion_tokens");
            usage.total_tokens = num("total_tokens");
            // Some gateways use Anthropic-style names; fold them in only
            // when the OpenAI keys were absent (008 R2 divergence rule).
            if (usage.prompt_tokens == 0)
                usage.prompt_tokens = num("input_tokens");
            if (usage.completion_tokens == 0)
                usage.completion_tokens = num("output_tokens");
            if (usage.total_tokens == 0)
                usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
            // v4.2: reasoning output tokens (OpenAI
            // completion_tokens_details.reasoning_tokens; some gateways
            // nest under usage.completion_token_details -- try both).
            for (const char* details_key :
                 {"completion_tokens_details", "completion_token_details"}) {
                if (auto details = u->find(details_key);
                    details != u->end() && details->is_object()) {
                    if (auto rt = details->find("reasoning_tokens"); rt != details->end() &&
                                                                     rt->is_number_integer() &&
                                                                     rt->get<long long>() > 0) {
                        usage.reasoning_tokens = rt->get<long long>();
                        reasoning.seen = true;
                        reasoning.has_reasoning_tokens = true;
                    }
                    break;
                }
            }
        }
        if (choices == parsed.end() || !choices->is_array() || choices->empty()) {
            return;
        }
        const auto& first = choices->front();
        const auto delta = first.find("delta");
        if (delta != first.end() && delta->is_object()) {
            // content text -> UI stream + accumulated reply
            if (auto content = delta->find("content");
                content != delta->end() && content->is_string()) {
                const auto fragment = content->get<std::string>();
                if (!fragment.empty()) {
                    full.append(fragment);
                    on_delta(fragment);
                }
            }
            // tool call fragments -> index-keyed accumulator (no UI feed:
            // 007 5.6 event/render separation -- tool noise must not
            // spam the reply area)
            if (auto tc = delta->find("tool_calls"); tc != delta->end() && tc->is_array()) {
                tool_calls.feed(*tc);
            }
            if (auto fc = delta->find("function_call"); fc != delta->end() && fc->is_object()) {
                tool_calls.feed_legacy(*fc);
            }
            // v4.2: reasoning content deltas prove the model actually
            // thought this round. Displayed nowhere yet (design 010 §11:
            // status reading only); never folded into the reply text.
            for (const char* key : {"reasoning_content", "reasoning"}) {
                if (auto rc = delta->find(key); rc != delta->end() && rc->is_string() &&
                                                !rc->get_ref<const std::string&>().empty()) {
                    reasoning.seen = true;
                    break;
                }
            }
        }
        // Non-streaming delivery inside the SSE stream: complete call
        // under choices[0].message (qwen compat variant, 2026-11-04).
        if (auto msg = first.find("message"); msg != first.end() && msg->is_object()) {
            if (auto tc = msg->find("tool_calls"); tc != msg->end() && tc->is_array())
                tool_calls.feed_message(*tc);
            if (auto fc = msg->find("function_call"); fc != msg->end() && fc->is_object())
                tool_calls.feed_legacy(*fc);
        }
        if (auto fr = first.find("finish_reason"); fr != first.end() && fr->is_string()) {
            finish_reason = fr->get<std::string>();
        }
    };
    CurlWriteCtx ctx{&lines, &should_abort};

    long status = 0;
    char error_buffer[CURL_ERROR_SIZE] = {0};
    ::curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
    ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                       static_cast<long>(config_.connect_timeout_seconds));
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config_.total_timeout_seconds));
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Never enable CURLOPT_VERBOSE in this code path: it would echo the
    // Authorization header (design 007 section 6).

    const CURLcode code = ::curl_easy_perform(curl);
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);

    if (should_abort()) {
        throw ProviderError(ProviderErrorKind::Cancelled, "request cancelled");
    }
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        throw ProviderError(ProviderErrorKind::Cancelled, "request cancelled");
    }
    if (code != CURLE_OK) {
        // error_buffer may contain the URL but never our API key (it is
        // header-only); still avoid pasting raw curl text blindly.
        std::string detail = error_buffer[0] ? error_buffer : ::curl_easy_strerror(code);
        throw ProviderError(ProviderErrorKind::Network, "network error: " + detail);
    }
    if (status < 200 || status >= 300) {
        http_error = "HTTP status " + std::to_string(status);
        throw ProviderError(ProviderErrorKind::HttpStatus, http_error, static_cast<int>(status));
    }
    CompletionResult result;
    result.text = std::move(full);
    result.tool_calls = tool_calls.finish();
    result.usage = usage;
    result.reasoning = std::move(reasoning);
    // Fail LOUD where an earlier build stayed silent: the stream carried
    // tool_call fragments (or finish_reason "tool_calls") but nothing
    // completed -- a gateway shape we do not parse (fragments without
    // id/name). Blank "AI:" replies are exactly that silent drop
    // (deepwiki live test 2026-11-04), so surface it as an error instead.
    if (result.tool_calls.empty() &&
        (tool_calls.saw_fragments() || finish_reason == "tool_calls" ||
         finish_reason == "function_call") &&
        result.text.empty()) {
        throw ProviderError(ProviderErrorKind::Protocol,
                            "provider streamed tool calls in an unrecognized shape (no id/name "
                            "resolved); stream structure [" +
                                tool_calls.shape_summary() +
                                "] slots=" + tool_calls.resolution_summary() +
                                " -- report this line to adapt the parser, or try --tools off");
    }
    return result;
}

std::optional<std::vector<std::string>> parse_models_json(std::string_view body,
                                                          std::string* error) {
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "models response is not a JSON object";
        return std::nullopt;
    }
    const auto data = parsed.find("data");
    if (data == parsed.end() || !data->is_array()) {
        if (error)
            *error = "models response missing data array";
        return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& entry : *data) {
        if (!entry.is_object())
            continue; // skip malformed entries
        const auto id = entry.find("id");
        if (id != entry.end() && id->is_string())
            ids.push_back(id->get<std::string>());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

long long estimate_context_tokens(std::string_view text) {
    // Coarse character-class estimate (v4 fallback when a gateway sends no
    // usage chunk). Walk UTF-8 codepoints: ASCII letters/digits/punct run
    // at ~4 chars/token, CJK & fullwidth at ~1.5 chars each, everything
    // else ~3. Never exact -- callers must mark results with `~`.
    long long ascii_chars = 0;
    long long cjk_codepoints = 0;
    long long other_codepoints = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if (b0 < 0x80) {
            ++ascii_chars;
        } else {
            if ((b0 >> 5) == 0b110)
                len = 2;
            else if ((b0 >> 4) == 0b1110)
                len = 3;
            else if ((b0 >> 3) == 0b11110)
                len = 4;
            // U+4E00..U+9FFF, U+3040..U+30FF (kana), U+AC00..U+D7AF (hangul),
            // U+FF00..U+FFEF (fullwidth): decode only 3-byte range cheaply.
            const bool likely_cjk = len == 3 && b0 >= 0xE3 && b0 <= 0xEF;
            if (likely_cjk)
                ++cjk_codepoints;
            else
                ++other_codepoints;
        }
        i += len;
    }
    const long long tokens = ascii_chars / 4 + cjk_codepoints + other_codepoints / 3;
    return tokens > 0 ? tokens : (text.empty() ? 0 : 1);
}

ProbeOutcome parse_context_probe_error(std::string_view body) {
    ProbeOutcome outcome;
    // Sanity bounds: real windows live between 1k and ~200M tokens; a
    // parse outside this range means we matched a wrong number (a byte
    // count, a retry-after, ...), so prefer no answer over a wrong one.
    auto in_range = [](long long n) {
        constexpr long long lo = 1000, hi = 200'000'000;
        return n >= lo && n <= hi;
    };
    // The message may arrive as a JSON error envelope; extract the human
    // text first so numeric JSON fields (an echoed "max_tokens": 1e9
    // request value) can never be mistaken for the limit. Fall back to
    // the raw body for non-JSON error pages.
    std::string text;
    {
        auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_discarded()) {
            if (auto err = parsed.find("error"); err != parsed.end() && err->is_object()) {
                if (auto m = err->find("message"); m != err->end() && m->is_string())
                    text = m->get<std::string>();
            }
        }
        if (text.empty())
            text.assign(body);
    }
    const auto digits_after = [&text, &in_range](std::size_t from) -> std::optional<long long> {
        const auto s = text.find_first_of("0123456789", from);
        if (s == std::string::npos)
            return std::nullopt;
        std::size_t e = s;
        long long value = 0;
        while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e]))) {
            value = value * 10 + (text[e] - '0');
            ++e;
        }
        if (!in_range(value))
            return std::nullopt;
        return value;
    };
    // 1) "maximum context length is 32768 tokens" (vLLM, OpenAI): TOTAL
    //    window -- the correct denominator for the ctx reading.
    if (auto pos = text.find("maximum context length is"); pos != std::string::npos) {
        if (auto value = digits_after(pos)) {
            outcome.kind = ProbeLimitKind::Window;
            outcome.value = *value;
            return outcome;
        }
    }
    // 2) "Range of <param> should be [1, N]" (DashScope family): the LAST
    //    digit run inside the brackets is the upper bound (the first is
    //    the lower 1). <param> decides the KIND: an input-length range is
    //    a usable window proxy; a max_tokens range is the OUTPUT cap and
    //    must never be stored as the context window (maintainer data:
    //    qwen3.8-flash input 983616 vs output cap 131072).
    if (auto pos = text.find("Range of"); pos != std::string::npos) {
        const auto open = text.find('[', pos);
        const auto close = open == std::string::npos ? std::string::npos : text.find(']', open);
        if (open != std::string::npos && close != std::string::npos) {
            std::optional<long long> last;
            for (std::size_t i = open + 1; i < close;) {
                if (std::isdigit(static_cast<unsigned char>(text[i]))) {
                    long long value = 0;
                    while (i < close && std::isdigit(static_cast<unsigned char>(text[i]))) {
                        value = value * 10 + (text[i] - '0');
                        ++i;
                    }
                    if (in_range(value))
                        last = value;
                } else {
                    ++i;
                }
            }
            if (last) {
                const std::string param(text, pos + 8, open - (pos + 8));
                const bool output = param.find("max_tokens") != std::string::npos ||
                                    param.find("max_output") != std::string::npos ||
                                    param.find("output") != std::string::npos;
                const bool input = param.find("input") != std::string::npos ||
                                   param.find("prompt") != std::string::npos;
                if (output) {
                    outcome.kind = ProbeLimitKind::OutputCap;
                    outcome.value = *last;
                } else if (input) {
                    outcome.kind = ProbeLimitKind::InputCap;
                    outcome.value = *last;
                } // else ambiguous param name: stay Unknown (never guess)
                return outcome;
            }
        }
    }
    // 3) "max_tokens: 1000000 > 131072" (OpenAI): the per-request OUTPUT
    //    cap. On a tiny prompt that is NOT the window -- classify honestly.
    if (auto pos = text.find("max_tokens:"); pos != std::string::npos) {
        const auto gt = text.find('>', pos);
        if (gt != std::string::npos) {
            if (auto value = digits_after(gt)) {
                outcome.kind = ProbeLimitKind::OutputCap;
                outcome.value = *value;
                return outcome;
            }
        }
    }
    return outcome; // Unknown, value 0
}

ProbeOutcome Provider::probe_context_window() {
    throw ProviderError(ProviderErrorKind::Protocol, "provider does not support window probing");
}

std::vector<std::string> Provider::list_models() {
    throw ProviderError(ProviderErrorKind::Protocol, "provider does not support model listing");
}

std::vector<ModelCapabilities> Provider::list_model_capabilities() {
    throw ProviderError(ProviderErrorKind::Protocol,
                        "provider does not support model capability listing");
}

namespace {
// Shared GET {base_url}/models transport for list_models and
// list_model_capabilities (v4.2): returns the response body; throws
// ProviderError on network/HTTP failure. The api_key only ever enters the
// Authorization header (never the returned body handling or error text).
std::string fetch_models_body(const ProviderConfig& config) {
    if (config.base_url.empty() || config.api_key.empty()) {
        throw ProviderError(ProviderErrorKind::Protocol, "provider config incomplete");
    }
    CURL* curl = ::curl_easy_init();
    if (!curl) {
        throw ProviderError(ProviderErrorKind::Network, "curl init failed");
    }
    const std::string url = config.base_url;
    const auto trimmed = url.find_last_not_of('/');
    std::string endpoint = trimmed == std::string::npos ? url : url.substr(0, trimmed + 1);
    endpoint += "/models";

    const std::string auth = "Authorization: Bearer " + config.api_key;
    const std::string accept = "Accept: application/json";
    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, auth.c_str());
    headers = ::curl_slist_append(headers, accept.c_str());

    std::string body;
    char error_buffer[CURL_ERROR_SIZE] = {0};
    ::curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    ::curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_accumulate);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                       static_cast<long>(config.connect_timeout_seconds));
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config.total_timeout_seconds));
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Never enable CURLOPT_VERBOSE here: it would echo the Authorization
    // header (design 007 section 6).

    const CURLcode code = ::curl_easy_perform(curl);
    long status = 0;
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : ::curl_easy_strerror(code);
        throw ProviderError(ProviderErrorKind::Network, "network error: " + detail);
    }
    if (status < 200 || status >= 300) {
        throw ProviderError(ProviderErrorKind::HttpStatus, "HTTP status " + std::to_string(status),
                            static_cast<int>(status));
    }
    if (config.debug_aux) // v4.6: level-2 only
        debug_dump_aux(config.debug_log_path, "MODELS RESPONSE " + std::to_string(status), body);
    return body;
}
} // namespace

std::optional<std::vector<ModelCapabilities>> parse_models_meta_json(std::string_view body,
                                                                     std::string* error) {
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error)
            *error = "models response is not a JSON object";
        return std::nullopt;
    }
    const auto data = parsed.find("data");
    if (data == parsed.end() || !data->is_array()) {
        if (error)
            *error = "models response missing data array";
        return std::nullopt;
    }
    std::vector<ModelCapabilities> caps;
    for (const auto& entry : *data) {
        if (!entry.is_object())
            continue;
        const auto id = entry.find("id");
        if (id == entry.end() || !id->is_string())
            continue; // same skip rule as parse_models_json
        ModelCapabilities model;
        model.id = id->get<std::string>();
        if (auto cl = entry.find("context_length");
            cl != entry.end() && cl->is_number_integer() && cl->get<long long>() > 0)
            model.context_length = cl->get<long long>();
        // OpenRouter shape: reasoning = {mandatory, default_enabled,
        // supported_efforts[], default_effort}. Any other shape (a bare
        // bool like some gateways use) only flips supports_reasoning.
        if (auto r = entry.find("reasoning"); r != entry.end()) {
            if (r->is_object()) {
                model.supports_reasoning = true;
                if (auto m = r->find("mandatory"); m != r->end() && m->is_boolean())
                    model.reasoning_mandatory = m->get<bool>();
                if (auto d = r->find("default_effort"); d != r->end() && d->is_string())
                    model.reasoning_default_effort = d->get<std::string>();
                if (auto se = r->find("supported_efforts"); se != r->end() && se->is_array()) {
                    for (const auto& level : *se)
                        if (level.is_string())
                            model.reasoning_efforts.push_back(level.get<std::string>());
                }
            } else if (r->is_boolean() && r->get<bool>()) {
                model.supports_reasoning = true;
            }
        }
        caps.push_back(std::move(model));
    }
    std::sort(caps.begin(), caps.end(),
              [](const ModelCapabilities& a, const ModelCapabilities& b) { return a.id < b.id; });
    caps.erase(std::unique(caps.begin(), caps.end(),
                           [](const ModelCapabilities& a, const ModelCapabilities& b) {
                               return a.id == b.id;
                           }),
               caps.end());
    return caps;
}

std::vector<std::string> OpenAiProvider::list_models() {
    const std::string body = fetch_models_body(config_);
    std::string parse_error;
    auto ids = parse_models_json(body, &parse_error);
    if (!ids) {
        // A 2xx that failed to parse still proves the credentials were
        // accepted: carry no status so callers report the key as verified
        // rather than as a login failure (fetch_models_body already threw
        // for non-2xx).
        throw ProviderError(ProviderErrorKind::Protocol, parse_error);
    }
    return *ids;
}

std::vector<ModelCapabilities> OpenAiProvider::list_model_capabilities() {
    const std::string body = fetch_models_body(config_);
    std::string parse_error;
    auto caps = parse_models_meta_json(body, &parse_error);
    if (!caps) {
        throw ProviderError(ProviderErrorKind::Protocol, parse_error);
    }
    return *caps;
}

namespace {
// One non-streaming chat POST for probing: returns (http_status, body).
// Network/config failures throw like the rest of the transport.
std::pair<long, std::string> probe_post_chat(const ProviderConfig& config,
                                             const nlohmann::json& request) {
    CURL* curl = ::curl_easy_init();
    if (!curl)
        throw ProviderError(ProviderErrorKind::Network, "curl init failed");
    const std::string url = config.base_url;
    const auto trimmed = url.find_last_not_of('/');
    std::string endpoint = trimmed == std::string::npos ? url : url.substr(0, trimmed + 1);
    endpoint += "/chat/completions";
    const std::string body = request.dump();
    const std::string auth = "Authorization: Bearer " + config.api_key;
    const std::string type = "Content-Type: application/json";
    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, auth.c_str());
    headers = ::curl_slist_append(headers, type.c_str());
    std::string response_body;
    char error_buffer[CURL_ERROR_SIZE] = {0};
    ::curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
    ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_accumulate);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                       static_cast<long>(config.connect_timeout_seconds));
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const CURLcode code = ::curl_easy_perform(curl);
    long status = 0;
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : ::curl_easy_strerror(code);
        throw ProviderError(ProviderErrorKind::Network, "network error: " + detail);
    }
    // v4.6: level-2 only -- probe traffic (request AND response) lands in
    // the debug file truncated, which is exactly what diagnosing the
    // unknown-gateway-wording case needs.
    if (config.debug_aux) {
        debug_dump_aux(config.debug_log_path, "PROBE REQUEST", body);
        debug_dump_aux(config.debug_log_path, "PROBE RESPONSE " + std::to_string(status),
                       response_body);
    }
    return {status, response_body};
}
} // namespace

ProbeOutcome OpenAiProvider::probe_context_window() {
    if (config_.base_url.empty() || config_.api_key.empty() || config_.model.empty())
        throw ProviderError(ProviderErrorKind::Protocol, "provider config incomplete");

    // Stage 1: tiny prompt + absurd max_tokens. Gateways validate before
    // billing, so a rejection is free and carries a bound. Which bound
    // depends on the validator that fires first (v4.3.1): vLLM/OpenAI
    // report the total window; DashScope-family reports the OUTPUT cap,
    // which is NOT a usable ctx denominator.
    nlohmann::json req;
    req["model"] = config_.model;
    req["messages"] = {{{"role", "user"}, {"content", "ping"}}};
    req["max_tokens"] = 1000000000;
    req["stream"] = false;
    const auto [status1, body1] = probe_post_chat(config_, req);
    if (status1 >= 200 && status1 < 300)
        return {}; // silent clamp: nothing learnable, never parse a success
    auto first = parse_context_probe_error(body1);
    if (first.kind == ProbeLimitKind::Window || first.kind == ProbeLimitKind::InputCap)
        return first; // the maintainer's preferred reading is already in hand

    // Stage 2 (v4.5, design 010 section 16): stage 1 only proved the
    // OUTPUT cap, so chase the INPUT bound -- the one that matters for
    // the ctx reading. One oversized prompt with max_tokens=1: the input
    // validator fires before any real generation, so the rejection is
    // again free. ~300k words ~= 400k tokens exceeds every current
    // window's input side while staying under request-size ceilings.
    nlohmann::json req2;
    req2["model"] = config_.model;
    req2["messages"] = {{{"role", "user"}, {"content", std::string(2400000, 'a')}}};
    req2["max_tokens"] = 1;
    req2["stream"] = false;
    const auto [status2, body2] = probe_post_chat(config_, req2);
    auto second =
        (status2 >= 200 && status2 < 300) ? ProbeOutcome{} : parse_context_probe_error(body2);
    if (second.kind == ProbeLimitKind::Window || second.kind == ProbeLimitKind::InputCap)
        return second;
    // Nothing better found: fall back to stage 1's honest OutputCap so
    // the caller can still tell the user what was learned.
    return first;
}

} // namespace mini_agent
