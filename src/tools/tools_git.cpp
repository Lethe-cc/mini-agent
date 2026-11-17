#include "mini_agent/tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/diff.hpp"

namespace mini_agent {
void GitRuntime::ensure_probed() const {
    if (probed_)
        return;
    probed_ = true;
    if (workspace_.empty()) {
        alive_ = false;
        return;
    }
    // Two-in-one probe: git runs, and the workspace is inside a work tree.
    // --no-optional-locks keeps the probe from touching repo state.
    const std::string ws = workspace_.string();
    const std::vector<const char*> argv = {"git",
                                           "-C",
                                           ws.c_str(),
                                           "--no-pager",
                                           "--no-optional-locks",
                                           "rev-parse",
                                           "--is-inside-work-tree",
                                           nullptr};
    SubprocessSpec spec;
    spec.exec_path = "git";
    spec.argv = &argv;
    spec.timeout_seconds = 5;
    auto result = run_subprocess(spec, [] { return false; });
    alive_ = !result.is_error && result.text.find("true") != std::string::npos;
}

bool GitRuntime::alive() const {
    ensure_probed();
    return alive_;
}

std::vector<ToolDefinition> GitRuntime::list() {
    std::vector<ToolDefinition> out;
    if (!alive())
        return out; // dead source: no tools advertised (008 5.5 posture)
    auto add = [&out](const char* name, const char* description, const char* schema) {
        ToolDefinition tool;
        tool.name = name;
        tool.description = description;
        tool.input_schema_json = schema;
        tool.source = "builtin-git";
        out.push_back(tool);
    };
    add("git_status",
        "Show workspace repository status (git status --porcelain=v1 -b plus a readable "
        "summary). Read-only. Arguments: {}.",
        R"({"type":"object","properties":{}})");
    add("git_diff",
        "Show the working-tree diff. Arguments: {\"staged\": optional true to diff the "
        "index, \"path\": optional file/dir filter (workspace-relative)}.",
        R"({"type":"object","properties":{"staged":{"type":"boolean"},"path":{"type":"string"}}})");
    add("git_log",
        "Show recent commits as one line each. Arguments: {\"limit\": optional 1-100, "
        "default 20}.",
        R"({"type":"object","properties":{"limit":{"type":"integer"}}})");
    add("git_add",
        "Stage workspace paths (git add -- <path>); paths must be inside the workspace. "
        "Arguments: {\"path\": \"file or directory path\"}.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})");
    add("git_commit",
        "Create a LOCAL commit from staged changes (git commit; never pushes). The "
        "approval view shows the file list and diff before anything happens. Arguments: "
        "{\"message\": \"subject line\", \"body\": optional paragraph text}.",
        R"({"type":"object","properties":{"message":{"type":"string"},"body":{"type":"string"}},"required":["message"]})");
    return out;
}

ToolResult GitRuntime::run_git(const std::vector<std::string>& args,
                               const ShouldAbort& should_abort) const {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& part : args)
        argv.push_back(part.c_str());
    argv.push_back(nullptr);
    SubprocessSpec spec;
    spec.exec_path = "git";
    spec.argv = &argv;
    spec.workspace = workspace_;
    spec.timeout_seconds = timeout_seconds_;
    return run_subprocess(spec, should_abort);
}

bool parse_porcelain_line(std::string_view line, std::string* status, std::string* path) {
    // Porcelain v1: "XY path" (X=index status, Y=worktree status). Rename
    // entries carry " -> " with NUL quoting disabled; we pass -z nowhere,
    // so quote-escapes may appear -- acceptable for display/preview.
    if (line.size() < 4 || line[0] == '#' || line[0] == '!')
        return false;
    const bool space = line[2] == ' ';
    if (!space)
        return false;
    *status = std::string(line.substr(0, 2));
    *path = std::string(line.substr(3));
    return !path->empty();
}

namespace {

// Base helper: common git flags for every invocation.
std::vector<std::string> git_base(const std::filesystem::path& workspace) {
    return {"git", "-C", workspace.string(), "--no-pager", "--no-optional-locks"};
}

} // namespace

std::string GitRuntime::preview(const ToolCall& call) {
    if (call.name != "git_commit" || !alive())
        return {};
    // Read-only preview: what WOULD be committed. Unstaged changes are
    // listed too so the human sees the gap (git commit takes staged only).
    std::string out;
    auto status_args = git_base(workspace_);
    status_args.insert(status_args.end(), {"status", "--porcelain=v1"});
    const auto status = run_git(status_args, [] { return false; });
    if (status.is_error)
        return {};
    std::size_t staged_count = 0;
    std::size_t unstaged_count = 0;
    std::vector<std::string> staged_files;
    std::istringstream stream(status.text);
    std::string line;
    while (std::getline(stream, line)) {
        std::string code;
        std::string file;
        if (!parse_porcelain_line(line, &code, &file))
            continue;
        const bool staged_change = code[0] != ' ' && code[0] != '?';
        const bool unstaged_change = code[1] != ' ';
        if (staged_change) {
            ++staged_count;
            staged_files.push_back(file);
        }
        if (unstaged_change)
            ++unstaged_count;
    }
    if (staged_count == 0)
        return "(nothing staged -- git_commit would fail unless git add runs first; " +
               std::to_string(unstaged_count) + " unstaged change(s) exist)";
    out += "commit will include " + std::to_string(staged_count) + " staged file(s)";
    if (unstaged_count)
        out += " (" + std::to_string(unstaged_count) + " other changes stay unstaged)";
    out += ":\n";
    for (std::size_t i = 0; i < staged_files.size() && i < 30; ++i)
        out += "  " + staged_files[i] + "\n";
    if (staged_files.size() > 30)
        out += "  ... +" + std::to_string(staged_files.size() - 30) + " more\n";
    // Staged diff stat for the eyes (budget-limited, 009 5.3 posture).
    auto diff_args = git_base(workspace_);
    diff_args.insert(diff_args.end(), {"diff", "--cached", "--stat", "--no-color"});
    const auto diff = run_git(diff_args, [] { return false; });
    if (!diff.is_error && diff.text != "(no output)") {
        out += diff.text;
    }
    return out;
}

ToolResult GitRuntime::call(const ToolCall& call, const ShouldAbort& should_abort) {
    if (!alive())
        return ToolResult{"git unavailable (binary missing or not a repository)", true};
    auto parsed = nlohmann::json::parse(call.arguments_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return ToolResult{"arguments must be a JSON object", true};
    auto abort_check = [&]() -> std::optional<ToolResult> {
        if (should_abort())
            return ToolResult{"aborted by user", true};
        return std::nullopt;
    };

    if (call.name == "git_status") {
        auto args = git_base(workspace_);
        args.insert(args.end(), {"status", "--porcelain=v1", "--branch"});
        return run_git(args, should_abort);
    }
    if (call.name == "git_diff") {
        auto args = git_base(workspace_);
        args.push_back("diff");
        args.push_back("--no-color");
        if (parsed.value("staged", false))
            args.push_back("--cached");
        if (parsed.contains("path")) {
            if (!parsed["path"].is_string())
                return ToolResult{"'path' must be a string", true};
            std::filesystem::path resolved;
            std::string error;
            if (!resolve_within_workspace(workspace_, parsed["path"].get<std::string>(), &resolved,
                                          &error))
                return ToolResult{"diff path: " + error, true};
            args.push_back("--");
            args.push_back(parsed["path"].get<std::string>());
        }
        return run_git(args, should_abort);
    }
    if (call.name == "git_log") {
        int64_t limit = 20;
        if (parsed.contains("limit")) {
            if (!parsed["limit"].is_number_integer() || parsed["limit"].get<int64_t>() < 1 ||
                parsed["limit"].get<int64_t>() > 100)
                return ToolResult{"'limit' must be an integer in 1..100", true};
            limit = parsed["limit"].get<int64_t>();
        }
        auto args = git_base(workspace_);
        args.insert(args.end(), {"log", "--oneline", "--decorate=no", "-" + std::to_string(limit)});
        return run_git(args, should_abort);
    }
    if (call.name == "git_add") {
        if (!parsed.contains("path") || !parsed["path"].is_string())
            return ToolResult{"git_add requires string 'path'", true};
        const std::string path_raw = parsed["path"].get<std::string>();
        std::filesystem::path resolved;
        std::string error;
        if (!resolve_within_workspace(workspace_, path_raw, &resolved, &error))
            return ToolResult{"add path: " + error, true};
        if (auto early = abort_check())
            return *early;
        auto args = git_base(workspace_);
        args.insert(args.end(), {"add", "--", path_raw});
        auto result = run_git(args, should_abort);
        if (!result.is_error)
            result.text = "staged: " + path_raw;
        return result;
    }
    if (call.name == "git_commit") {
        if (!parsed.contains("message") || !parsed["message"].is_string() ||
            parsed["message"].get<std::string>().empty())
            return ToolResult{"git_commit requires non-empty string 'message'", true};
        std::string message = parsed["message"].get<std::string>();
        if (message.find('\n') != std::string::npos)
            return ToolResult{"'message' must be a single subject line; put paragraphs in 'body'",
                              true};
        std::string body;
        if (parsed.contains("body")) {
            if (!parsed["body"].is_string())
                return ToolResult{"'body' must be a string", true};
            body = parsed["body"].get<std::string>();
        }
        if (auto early = abort_check())
            return *early;
        // Fixed argv: subject via -m, optional body via second -m. Model
        // text NEVER forms a command, only a dedicated argument slot.
        auto args = git_base(workspace_);
        args.insert(args.end(), {"commit", "-m", message});
        if (!body.empty()) {
            args.push_back("-m");
            args.push_back(body);
        }
        auto result = run_git(args, should_abort);
        if (!result.is_error) {
            // Compact the verbose default output to the summary line.
            auto log_args = git_base(workspace_);
            log_args.insert(log_args.end(), {"log", "-1", "--oneline"});
            auto head = run_git(log_args, should_abort);
            result.text = head.is_error ? "committed" : ("committed: " + head.text);
        }
        return result;
    }
    if (call.name == "git_push" || call.name == "git_pull" || call.name == "git_fetch" ||
        call.name == "git_reset" || call.name == "git_rebase" || call.name == "git_checkout") {
        return ToolResult{call.name +
                              ": not provided -- remote and history-rewriting operations are "
                              "human-only (ADR 008); ask the user to run them",
                          true};
    }
    return ToolResult{"unknown git tool: " + call.name, true};
}

} // namespace mini_agent
