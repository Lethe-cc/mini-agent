#include "mini_agent/provider.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
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

std::string request_body(const Chat& chat, const std::string& model,
                         const std::string& tools_json) {
    nlohmann::json root;
    root["model"] = model;
    root["stream"] = true;
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
                                      const std::string& tools_json) {
    return request_body(chat, model, tools_json);
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
    return CompletionResult{complete(chat, on_delta, should_abort), {}};
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

    const std::string body = request_body(chat, config_.model, tools_json);
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
    ToolCallAccumulator tool_calls;
    SseLines lines;
    lines.on_data_ = [&](std::string&& payload) {
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

std::vector<std::string> Provider::list_models() {
    throw ProviderError(ProviderErrorKind::Protocol, "provider does not support model listing");
}

std::vector<std::string> OpenAiProvider::list_models() {
    if (config_.base_url.empty() || config_.api_key.empty()) {
        throw ProviderError(ProviderErrorKind::Protocol, "provider config incomplete");
    }
    CURL* curl = ::curl_easy_init();
    if (!curl) {
        throw ProviderError(ProviderErrorKind::Network, "curl init failed");
    }
    const std::string url = config_.base_url;
    const auto trimmed = url.find_last_not_of('/');
    std::string endpoint = trimmed == std::string::npos ? url : url.substr(0, trimmed + 1);
    endpoint += "/models";

    const std::string auth = "Authorization: Bearer " + config_.api_key;
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
                       static_cast<long>(config_.connect_timeout_seconds));
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config_.total_timeout_seconds));
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
    std::string parse_error;
    auto ids = parse_models_json(body, &parse_error);
    if (!ids) {
        // 2xx means the credentials were accepted even though the body was
        // unusable: carry the status so the caller can still report the key
        // as verified rather than as a login failure.
        throw ProviderError(ProviderErrorKind::Protocol, parse_error, static_cast<int>(status));
    }
    return *ids;
}

} // namespace mini_agent
