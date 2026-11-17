#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "mini_agent/diff.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace mini_agent {

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::add(std::unique_ptr<ToolRuntime> runtime) {
    if (!runtime)
        return;
    // Do NOT call runtime->list() here: that would defeat lazy start
    // (design 008 section 5.5) -- an MCP client spawns/handshakes inside
    // its first list(), so eagerly listing would hit the network on every
    // launch. Name claiming happens in list() (late-registered below),
    // and find() scans runtimes directly. Startup stays network-free.
    runtimes_.push_back(std::move(runtime));
}

std::vector<ToolDefinition> ToolRegistry::list() {
    std::vector<ToolDefinition> out;
    for (auto& runtime : runtimes_) {
        if (!runtime->alive())
            continue; // dead source: tools vanish, loop continues (008 5.5)
        for (auto& definition : runtime->list()) {
            if (std::find(names_.begin(), names_.end(), definition.name) == names_.end()) {
                names_.push_back(definition.name); // late-registered name
            }
            out.push_back(std::move(definition));
        }
    }
    return out;
}

ToolRuntime* ToolRegistry::find(const std::string& name) {
    for (auto& runtime : runtimes_) {
        if (!runtime->alive())
            continue;
        for (const auto& definition : runtime->list()) {
            if (definition.name == name)
                return runtime.get();
        }
    }
    return nullptr;
}

std::string ToolRegistry::preview(const ToolCall& call) {
    auto* runtime = find(call.name);
    return runtime ? runtime->preview(call) : std::string();
}

void ToolRegistry::disarm(const ToolCall& call) {
    auto* runtime = find(call.name);
    if (runtime)
        runtime->disarm(call);
}

std::string ToolRegistry::result_preview(const ToolCall& call, const ToolResult& result) {
    auto* runtime = find(call.name);
    return runtime ? runtime->result_preview(call, result) : std::string();
}

std::string ToolRegistry::to_openai_tools_json() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& definition : list()) {
        nlohmann::json entry;
        entry["type"] = "function";
        entry["function"]["name"] = definition.name;
        entry["function"]["description"] = definition.description;
        auto schema = nlohmann::json::parse(definition.input_schema_json, nullptr, false);
        entry["function"]["parameters"] =
            schema.is_discarded() ? nlohmann::json{{"type", "object"}} : std::move(schema);
        arr.push_back(std::move(entry));
    }
    return arr.dump();
}

// ---------------------------------------------------------------------------
// run_bash argument parsing (fail-closed)
// ---------------------------------------------------------------------------

std::optional<BashRequest> parse_bash_arguments(std::string_view arguments_json,
                                                std::size_t default_timeout_seconds,
                                                std::string* error) {
    auto parsed = nlohmann::json::parse(arguments_json, nullptr, false);
    if (parsed.is_discarded()) {
        if (error)
            *error = "tool arguments are not valid JSON";
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        if (error)
            *error = "tool arguments must be a JSON object";
        return std::nullopt;
    }
    BashRequest request;
    request.timeout_seconds = default_timeout_seconds;
    auto command_it = parsed.find("command");
    if (command_it == parsed.end() || !command_it->is_string() ||
        command_it->get_ref<const std::string&>().empty()) {
        if (error)
            *error = "run_bash requires a non-empty string 'command'";
        return std::nullopt;
    }
    request.command = command_it->get<std::string>();
    if (auto timeout_it = parsed.find("timeout"); timeout_it != parsed.end()) {
        if (!timeout_it->is_number_integer() || timeout_it->get<int64_t>() <= 0 ||
            timeout_it->get<int64_t>() > 3600) {
            if (error)
                *error = "run_bash 'timeout' must be an integer in 1..3600 seconds";
            return std::nullopt;
        }
        request.timeout_seconds = static_cast<std::size_t>(timeout_it->get<int64_t>());
    }
    return request;
}

// Shared head+tail truncation marker (cap_tool_output + BoundedOutputSink
// must emit the identical byte sequence; design 008 5.2).

std::string sanitize_display(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b && i + 1 < text.size()) {
            // Drop one ANSI escape sequence: ESC [ ... final-byte,
            // ESC ] ... BEL/ST, or ESC + single letter.
            const char next = text[i + 1];
            if (next == '[') {
                std::size_t j = i + 2;
                while (j < text.size() && !(text[j] >= 0x40 && text[j] <= 0x7e)) // CSI final byte
                    ++j;
                i = (j < text.size()) ? j + 1 : text.size();
                continue;
            }
            if (next == ']') {
                std::size_t j = i + 2;
                while (j < text.size() && text[j] != '\x07' && !(text[j] == 0x1b))
                    ++j;
                i = (j < text.size()) ? j + 1 : text.size();
                continue;
            }
            i += 2; // two-char escape
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            ++i; // drop other C0 controls
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// BuiltinRuntime: run_bash via fork/exec /bin/bash -lc
// ---------------------------------------------------------------------------

std::vector<ToolDefinition> BuiltinRuntime::list() {
    std::vector<ToolDefinition> out;
    ToolDefinition bash;
    bash.name = "run_bash";
    bash.description =
        "Run a shell command with /bin/bash -lc and return merged stdout+stderr. "
        "Use for inspection and command execution. Arguments: {\"command\": \"...\", "
        "\"timeout\": optional seconds}.";
    bash.input_schema_json =
        R"({"type":"object","properties":{"command":{"type":"string","description":"shell command"},"timeout":{"type":"integer","description":"max seconds, 1-3600"}},"required":["command"]})";
    bash.source = "builtin";
    out.push_back(std::move(bash));
    return out;
}


ToolResult BuiltinRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    if (call.name != "run_bash") {
        return ToolResult{"unknown builtin tool: " + call.name, true};
    }
    std::string arg_error;
    auto request = parse_bash_arguments(call.arguments_json, default_timeout_seconds_, &arg_error);
    if (!request) {
        return ToolResult{arg_error, true};
    }
    const std::vector<const char*> argv = {"/bin/bash", "-lc", request->command.c_str(), nullptr};
    SubprocessSpec spec;
    spec.exec_path = "/bin/bash";
    spec.argv = &argv;
    spec.workspace = workspace_;
    spec.timeout_seconds = request->timeout_seconds;
    return run_subprocess(spec, should_abort);
}

// ---------------------------------------------------------------------------
// v3 workspace boundary + FileRuntime (design 009 section 5.1, ADR 007)
// ---------------------------------------------------------------------------

} // namespace mini_agent
