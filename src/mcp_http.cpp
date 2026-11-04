#include "mini_agent/mcp.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <mutex>
#include <sstream>

namespace mini_agent {
namespace {

// Streamable HTTP transport (design 008 section 5.2, spec 2025-06-18
// Transports re-verified 2026-11-02): every client message is one POST to
// the MCP endpoint; responses are either a single JSON body or an SSE
// stream that we drain until the matching response arrives. Requests
// carry Mcp-Session-Id (captured at initialize) and, from 2025-06-18,
// MCP-Protocol-Version. Auth: static bearer only (no OAuth per maintainer
// decision 2026-11-02). Server-initiated GET streams are not opened.

constexpr std::size_t kMaxBodyBytes = 8u * 1024u * 1024u;

size_t accumulate(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (out->size() + bytes > kMaxBodyBytes)
        return 0; // abort transfer: oversized body
    out->append(ptr, bytes);
    return bytes;
}

// Minimal header sink: we need exactly two response headers.
size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::string line(buffer, size * nitems);
    const auto lower = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(::tolower(c));
        return s;
    };
    if (line.rfind("Mcp-Session-Id:", 0) == 0 || lower(line).rfind("mcp-session-id:", 0) == 0) {
        const auto colon = line.find(':');
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        *out = value;
    }
    return size * nitems;
}

// Split a whole SSE body into data-payload strings (the non-streaming
// read model: curl hands us the complete body when the server closes the
// stream after responding, which spec-compliant single-response servers
// do per Transports section).
std::vector<std::string> sse_payloads(const std::string& body) {
    std::vector<std::string> out;
    std::istringstream stream(body);
    std::string line;
    std::string data;
    bool in_event = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) { // event boundary
            if (in_event && !data.empty())
                out.push_back(data);
            data.clear();
            in_event = false;
            continue;
        }
        if (line.rfind("data:", 0) == 0) {
            auto payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ')
                payload.erase(0, 1);
            if (!data.empty())
                data += '\n';
            data += payload;
            in_event = true;
        }
        // other fields (event:/id:/retry:) are irrelevant to us
    }
    if (in_event && !data.empty())
        out.push_back(data);
    return out;
}

class HttpTransport final : public McpTransport {
  public:
    explicit HttpTransport(McpServerConfig config) : config_(std::move(config)) {
    }
    ~HttpTransport() override {
        close(); // frees the persistent curl handle
    }

    bool start(std::string* error) override {
        if (config_.url.empty()) {
            if (error)
                *error = "http transport requires url";
            return false;
        }
        if (!started_) {
            started_ = true; // statelessness: real work happens per POST
        }
        return true;
    }

    bool alive() const override {
        return started_ && !dead_;
    }

    bool send(std::string_view message, std::string* /*error*/) override {
        pending_.assign(message);
        return true; // POST happens inside send_and_collect at request time
    }

    // The whole exchange per message: POST, classify the response, queue
    // any JSON-RPC messages. recv() then drains the queue. This matches
    // the abstract "response inline with request" model (008 5.2).
    bool send_and_collect(std::string* error) {
        queue_.clear();
        queue_pos_ = 0;
        const std::string body = pending_;
        pending_.clear();

        if (!curl_) {
            curl_ = ::curl_easy_init(); // persistent handle: connection and
            // TLS session reuse across a server's whole session
            // (keep-alive to remote endpoints; review 2026-11-05)
        }
        CURL* curl = curl_;
        if (!curl) {
            if (error)
                *error = "curl init failed";
            return false;
        }
        struct curl_slist* headers = nullptr;
        const std::string accept = "Accept: application/json, text/event-stream";
        const std::string type = "Content-Type: application/json";
        headers = ::curl_slist_append(headers, accept.c_str());
        headers = ::curl_slist_append(headers, type.c_str());
        std::string session_header, version_header;
        if (!session_id_.empty())
            session_header = "Mcp-Session-Id: " + session_id_;
        if (!negotiated_version_.empty())
            version_header = "MCP-Protocol-Version: " + negotiated_version_;
        if (!session_header.empty())
            headers = ::curl_slist_append(headers, session_header.c_str());
        if (!version_header.empty())
            headers = ::curl_slist_append(headers, version_header.c_str());
        std::string auth_header;
        if (!config_.bearer_token.empty())
            auth_header = "Authorization: Bearer " + config_.bearer_token;
        if (!auth_header.empty())
            headers = ::curl_slist_append(headers, auth_header.c_str());

        std::string response_body;
        std::string captured_session;
        char error_buffer[CURL_ERROR_SIZE] = {0};
        long status = 0;
        char* content_type = nullptr;
        ::curl_easy_setopt(curl, CURLOPT_URL, config_.url.c_str());
        ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, accumulate);
        ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        ::curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        ::curl_easy_setopt(curl, CURLOPT_HEADERDATA, &captured_session);
        ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        const CURLcode code = ::curl_easy_perform(curl);
        ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        ::curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
        // Copy BEFORE cleanup: the getinfo pointer is owned by the handle.
        const std::string ctype = content_type ? content_type : "";
        ::curl_slist_free_all(headers);
        ::curl_easy_reset(curl); // keep the handle; drop this request's opts

        if (code == CURLE_ABORTED_BY_CALLBACK) {
            if (error)
                *error = "mcp http response body exceeds 8 MiB";
            dead_ = true;
            return false;
        }
        if (code != CURLE_OK) {
            if (error)
                *error = std::string("mcp http network error: ") +
                         (error_buffer[0] ? error_buffer : ::curl_easy_strerror(code));
            return false;
        }
        if (!captured_session.empty())
            session_id_ = captured_session;
        if (status == 404 && !session_id_.empty()) {
            // Spec session rule: session expired -> client MUST re-init.
            session_id_.clear();
            negotiated_version_.clear();
            if (error)
                *error = "mcp session expired; re-initialize required";
            return false;
        }
        if (status == 401 || status == 403) {
            if (error)
                *error = "mcp server refused authentication (HTTP " + std::to_string(status) +
                         "); bearer token missing or invalid (OAuth not supported)";
            dead_ = true;
            return false;
        }
        if (status < 200 || status >= 300) {
            if (error)
                *error = "mcp http status " + std::to_string(status);
            return false;
        }
        if (status == 202 || response_body.empty()) {
            return true; // notification accepted, no messages
        }
        if (ctype.rfind("text/event-stream", 0) == 0) {
            for (auto& payload : sse_payloads(response_body))
                queue_.push_back(std::move(payload));
        } else {
            queue_.push_back(response_body); // single JSON object
        }
        return true;
    }

    std::optional<std::string> recv(std::chrono::milliseconds timeout,
                                    std::string* error) override {
        (void)timeout;
        (void)error;
        if (queue_pos_ < queue_.size())
            return queue_[queue_pos_++];
        return std::nullopt; // send_and_collect already awaited everything
    }

    void close() noexcept override {
        started_ = false;
        session_id_.clear();
        negotiated_version_.clear();
        if (curl_) {
            ::curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
    }

    // Records the negotiated protocol version so follow-up POSTs carry
    // MCP-Protocol-Version (2025-06-18 MUST).
    void set_negotiated_version(const std::string& version) {
        negotiated_version_ = version;
    }

  private:
    McpServerConfig config_;
    bool started_ = false;
    bool dead_ = false;
    std::string session_id_;
    std::string negotiated_version_;
    std::string pending_;
    std::vector<std::string> queue_;
    std::size_t queue_pos_ = 0;
    CURL* curl_ = nullptr;
};

// send() completes the round trip inline (Streamable HTTP posts are
// request/response pairs): collect the POST and its replies inside send,
// then recv drains the queue.
class HttpAdapter final : public McpTransport {
  public:
    explicit HttpAdapter(std::unique_ptr<HttpTransport> inner) : inner_(std::move(inner)) {
    }
    bool start(std::string* error) override {
        return inner_->start(error);
    }
    bool alive() const override {
        return inner_->alive();
    }
    bool send(std::string_view message, std::string* error) override {
        if (!inner_->send(message, error))
            return false;
        return inner_->send_and_collect(error);
    }
    std::optional<std::string> recv(std::chrono::milliseconds timeout,
                                    std::string* error) override {
        return inner_->recv(timeout, error);
    }
    void close() noexcept override {
        inner_->close();
    }
    void set_negotiated_version(const std::string& version) {
        inner_->set_negotiated_version(version);
    }

  private:
    std::unique_ptr<HttpTransport> inner_;
};

} // namespace

void set_http_transport_version(McpTransport& transport, const std::string& version) {
    if (auto* adapter = dynamic_cast<HttpAdapter*>(&transport))
        adapter->set_negotiated_version(version);
    else if (auto* direct = dynamic_cast<HttpTransport*>(&transport))
        direct->set_negotiated_version(version);
}

std::unique_ptr<McpTransport> make_http_transport(const McpServerConfig& config) {
    return std::make_unique<HttpAdapter>(std::make_unique<HttpTransport>(config));
}

} // namespace mini_agent
