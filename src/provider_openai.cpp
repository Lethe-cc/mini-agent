#include "mini_agent/provider.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <cstring>
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

// Extracts content delta text from one SSE JSON payload. Returns nullopt
// when the payload carries no content (roles-only deltas, tool fragments
// from future providers, heartbeats).
std::optional<std::string> delta_content(std::string_view payload) {
    if (payload == "[DONE]")
        return std::string{}; // sentinel: end, empty
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
        return std::nullopt;
    }
    const auto& first = choices->front();
    const auto delta = first.find("delta");
    if (delta == first.end() || !delta->is_object()) {
        return std::nullopt;
    }
    const auto content = delta->find("content");
    if (content == delta->end() || !content->is_string()) {
        return std::nullopt;
    }
    return content->get<std::string>();
}

std::string request_body(const Chat& chat, const std::string& model) {
    nlohmann::json root;
    root["model"] = model;
    root["stream"] = true;
    root["messages"] = nlohmann::json::parse(chat_to_json(chat));
    return root.dump();
}

} // namespace

std::string OpenAiProvider::complete(const Chat& chat,
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

    const std::string body = request_body(chat, config_.model);
    const std::string auth = "Authorization: Bearer " + config_.api_key;
    const std::string type = "Content-Type: application/json";
    const std::string accept = "Accept: text/event-stream";
    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, auth.c_str());
    headers = ::curl_slist_append(headers, type.c_str());
    headers = ::curl_slist_append(headers, accept.c_str());

    std::string full;
    std::string http_error;
    SseLines lines;
    lines.on_data_ = [&](std::string&& payload) {
        if (auto fragment = delta_content(payload)) {
            if (!fragment->empty()) {
                full.append(*fragment);
                on_delta(*fragment);
            }
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
        throw ProviderError(ProviderErrorKind::HttpStatus, http_error);
    }
    return full;
}

} // namespace mini_agent
