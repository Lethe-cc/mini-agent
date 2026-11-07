"""Stdlib-only mock OpenAI-compatible SSE endpoint for CLI smoke tests.

Dev tool only; NOT a business test gate. Usage:
  python3 tests/mock_openai_server.py 8931 &
  MINI_AGENT_BASE_URL=http://127.0.0.1:8931/v1 MINI_AGENT_API_KEY=test-key \
    MINI_AGENT_MODEL=mock mini-agent ask -
Echoes the last user message back over SSE. Binds 127.0.0.1 only.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


SCRIPTS = {
    # standard OpenAI split: id/name once, arguments streamed in pieces
    "TOOLTEST": [
        {"choices": [{"delta": {"role": "assistant"}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "call_1",
            "function": {"name": "run_bash", "arguments": ""}}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "function": {"arguments": '{"command":'}}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "function": {"arguments": '"echo hi from tool"}'}}]}}]},
        {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
    ],
    # flattened fragments: call_id + top-level name/arguments
    "VARFLAT": [
        {"choices": [{"delta": {"tool_calls": [{"index": 0, "call_id": "cfc1",
            "name": "run_bash", "arguments": ""}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "arguments": '{"command":"echo'}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "arguments": ' flat"}'}]}}]},
        {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
    ],
    # legacy single function_call stream
    "VARFC": [
        {"choices": [{"delta": {"function_call": {"name": "run_bash"}}}]},
        {"choices": [{"delta": {"function_call":
            {"arguments": '{"command":"echo legacy fc"}'}}}]},
        {"choices": [{"delta": {}, "finish_reason": "function_call"}]},
    ],
    # arguments delivered as a pre-parsed object inside the function wrapper
    "VAROBJ": [
        {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "cobj",
            "function": {"name": "run_bash",
                         "arguments": {"command": "echo obj args"}}}]}}]},
        {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
    ],
    # qwen/DashScope compat pattern (live fingerprint 2026-11-04):
    # leading EMPTY id/name placeholders + type field arrive first
    "VARQWEN": [
        {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "",
            "type": "function", "function": {"name": "", "arguments": ""}}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "call_qw1",
            "type": "function", "function": {"name": "run_bash",
                                             "arguments": ""}}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "function": {"arguments": '{"command":"'}}]}}]},
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "function": {"arguments": 'echo qwen"}'}}]}}]},
        {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
    ],
    # complete tool_calls under choices[0].message (message-level delivery)
    "VARMSG": [
        {"choices": [{"delta": {}, "message": {"tool_calls": [{"index": 0,
            "id": "call_m1", "type": "function", "function": {
                "name": "run_bash", "arguments": '{"command":"echo msg"}'}}]},
            "finish_reason": "tool_calls"}]},
    ],
    # tool from a CLI-injected http MCP server (dw__echo, 008 --mcp E2E)
    "INJECT": [
        {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "ci",
            "type": "function", "function": {"name": "dw__echo",
                "arguments": '{"text":"injected ok"}'}}]}}]},
        {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
    ],
    # v4.2 reasoning flow: reasoning_content deltas + effort echo +
    # reasoning_tokens in usage (all three actual-level evidence sources).
    "REASONTEST": [
        {"choices": [{"delta": {"reasoning_content": "thinking hard about it"}}]},
        {"choices": [{"delta": {"reasoning_content": ", step by step"}}]},
        {"choices": [{"delta": {"content": "answer after thought"},
                      "reasoning_effort": "high"}]},
        {"choices": [{"delta": {}, "finish_reason": "stop"}],
         "usage": {"prompt_tokens": 50, "completion_tokens": 20, "total_tokens": 70,
                   "completion_tokens_details": {"reasoning_tokens": 12}}},
    ],
    # unrecognized shape: fragments without any id/name -- client must
    # raise a protocol error with the structure fingerprint (008 D3)
    "BADSHAPE": [
        {"choices": [{"delta": {"tool_calls": [{"index": 0,
            "function": {"arguments": '{"a":1}'}}]}}]},
        {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
    ],
    # v3 coding flow (multi-round, keyed by tool-result count): read ->
    # edit -> git_status -> git_commit -> final summary.
    "CODEFIX": [
        [  # round 1: request read_file
            {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c1",
                "type": "function", "function": {"name": "read_file",
                "arguments": '{"path":"hello.txt"}'}}]}}]},
            {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
        ],
        [  # round 2: request edit_file
            {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c2",
                "type": "function", "function": {"name": "edit_file",
                "arguments": '{"path":"hello.txt","old_str":"CHANGEME","new_str":"changed by mini-agent"}'}}]}}]},
            {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
        ],
        [  # round 3: check git status
            {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c3",
                "type": "function", "function": {"name": "git_status",
                "arguments": '{}'}}]}}]},
            {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
        ],
        [  # round 4: stage + commit
            {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c4",
                "type": "function", "function": {"name": "git_add",
                "arguments": '{"path":"hello.txt"}'}}]}}]},
            {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
        ],
        [  # round 5
            {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c5",
                "type": "function", "function": {"name": "git_commit",
                "arguments": '{"message":"update hello.txt wording"}'}}]}}]},
            {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
        ],
        [  # round 6: final text
            {"choices": [{"delta": {"content": "done: edited and committed"}}]},
            {"choices": [{"delta": {}, "finish_reason": "stop"}]},
        ],
    ],
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Model listing endpoint (mini-agent /models and the TUI auto-list).
        if not self.path.rstrip("/").endswith("/models"):
            self.send_error(404)
            return
        if not self.headers.get("Authorization", "").startswith("Bearer "):
            self.send_error(401)
            return
        payload = {
            "object": "list",
            "data": [
                # v4.2: OpenRouter-style capability metadata on one entry,
                # plain OpenAI shape (id only) on the other.
                {"id": "mock-small", "object": "model"},
                {"id": "mock-large", "object": "model", "context_length": 131072,
                 "reasoning": {"mandatory": False, "default_enabled": False,
                               "supported_efforts": ["high", "medium", "low"],
                               "default_effort": "medium"}},
            ],
        }
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length))
        messages = body.get("messages", [])
        # v4.3 probe shape: an oversized max_tokens (non-streaming) gets
        # the DashScope-style range rejection the client parses.
        if body.get("max_tokens", 0) > 100000 and not body.get("stream"):
            payload = {"error": {"message": "<400> Range of max_tokens should be [1, 131072]"}}
            resp = json.dumps(payload).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            return
        # v4.5 stage-2 probe shape: an oversized prompt gets the INPUT
        # range rejection (DashScope fires the max_tokens validator first,
        # so the client must chase the input bound separately).
        if sum(len(str(m.get("content", ""))) for m in messages) > 1000000:
            payload = {"error": {"message": "<400> Range of input length should be [1, 983616]"}}
            resp = json.dumps(payload).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            return
        last_user = next(
            (m["content"] for m in reversed(messages) if m.get("role") == "user"), ""
        )
        if not self.headers.get("Authorization", "").startswith("Bearer "):
            self.send_error(401)
            return
        has_tools = bool(body.get("tools"))
        saw_tool_result = any(m.get("role") == "tool" for m in messages)
        prefix1 = ("TOOLTEST", "VARFLAT", "VARFC", "VAROBJ", "VARQWEN",
                   "VARMSG", "INJECT", "BADSHAPE", "REASONTEST")
        if last_user.startswith("CODEFIX") and has_tools:
            # v3 multi-round coding flow: pick the script by how many tool
            # results came back (read/edit/status/add/commit, then summary).
            tool_rounds = sum(1 for m in messages if m.get("role") == "tool")
            seq = SCRIPTS["CODEFIX"][min(tool_rounds, 5)]
        elif last_user.startswith(prefix1) and has_tools and not saw_tool_result:
            seq = SCRIPTS[next(k for k in prefix1 if last_user.startswith(k))]
        elif last_user.startswith(prefix1):
            # round 2: consume the tool result and finish
            tool_text = next((m["content"] for m in reversed(messages)
                              if m.get("role") == "tool"), "none")
            seq = [{"choices": [{"delta": {"content": f"tool said: {tool_text[:60]}"}}]},
                   {"choices": [{"delta": {}, "finish_reason": "stop"}]}]
        else:
            seq = [{"choices": [{"delta": {"content": "echo:"}}]},
                   {"choices": [{"delta": {"content": f" {last_user[:40]}"}}]},
                   {"choices": [{"delta": {}, "finish_reason": "stop"}]}]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for chunk in seq:
            self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
        # v4: OpenAI semantics -- the usage chunk only arrives when the
        # client asked for it via stream_options.include_usage, and it
        # carries an empty choices array.
        if (body.get("stream_options") or {}).get("include_usage"):
            usage = {
                "choices": [],
                "usage": {
                    "prompt_tokens": 123,
                    "completion_tokens": 45,
                    "total_tokens": 168,
                },
            }
            self.wfile.write(f"data: {json.dumps(usage)}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")

    def log_message(self, *args):
        pass  # keep stderr clean; never log request headers


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8931
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
