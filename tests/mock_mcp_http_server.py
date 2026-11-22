#!/usr/bin/env python3
"""Stdlib-only mock MCP server over Streamable HTTP, for v2 HttpTransport tests.

Dev tool only; NOT a business quality gate. Implements one MCP endpoint that
supports POST (JSON-RPC messages) with both response shapes from the spec:
  * application/json for notifications (202 Accepted, no body)
  * text/event-stream carrying the JSON-RPC response as one SSE event
Session management: returns Mcp-Session-Id on initialize and requires it
afterwards (400 without, 404 for unknown ids so the client's re-init path
is exercised). Binds 127.0.0.1 only.

--rich adds the M2a surface (design 022): resources/prompts capabilities,
two-page resources/list, resources/read, prompts/list + prompts/get, a
`notify` tool whose POST SSE response piggybacks resources/prompts
list_changed notifications before the result, and a GET SSE stream
(per-session, comment heartbeats) so the SDK's standing stream has a
peer. Default mode keeps the pre-M2a behaviour (GET -> 405).

Usage:  python3 tests/mock_mcp_http_server.py 8933 [--rich]
"""
import json
import sys
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RICH = "--rich" in sys.argv[1:]
TOOL_RESULTS = {
    "echo": lambda args: ("echoed: " + args.get("text", ""), False)
    if isinstance(args.get("text"), str) else ("missing string 'text'", True),
    "fail": lambda args: ("boom", True),
}
SESSIONS = set()

RESOURCES = [
    {"uri": "mock://doc/a", "name": "doc-a", "description": "first doc",
     "mimeType": "text/plain"},
    {"uri": "mock://doc/b", "name": "doc-b", "description": "second doc",
     "mimeType": "text/plain"},
    {"uri": "mock://doc/c", "name": "doc-c", "description": "third doc",
     "mimeType": "text/plain"},
]
RESOURCE_TEXTS = {
    "mock://doc/a": "alpha resource body",
    "mock://doc/b": "beta resource body",
    "mock://doc/c": "gamma resource body",
}
PROMPTS = [
    {
        "name": "review-code",
        "description": "Review the given code.\nMulti-line description.",
        "arguments": [
            {"name": "code", "description": "code under review", "required": True},
            {"name": "lang", "description": "language", "required": False},
        ],
    },
]


def rich_result(method, params):
    """Rich-mode request results; None = not a rich method.

    Returns (value, err) with exactly one side set.
    """
    if method == "resources/list":
        if params.get("cursor") == "page2":
            return {"resources": RESOURCES[2:]}, None
        return {"resources": RESOURCES[:2], "nextCursor": "page2"}, None
    if method == "resources/read":
        uri = params.get("uri")
        if uri in RESOURCE_TEXTS:
            return {"contents": [{"uri": uri, "mimeType": "text/plain",
                                  "text": RESOURCE_TEXTS[uri]}]}, None
        return None, {"code": -32602, "message": f"no such resource: {uri}"}
    if method == "prompts/list":
        return {"prompts": PROMPTS}, None
    if method == "prompts/get":
        if params.get("name") != "review-code":
            return None, {"code": -32602, "message": f"no such prompt: {params.get('name')}"}
        args = params.get("arguments") or {}
        code = args.get("code", "")
        lang = args.get("lang", "")
        return {
            "description": "rendered review prompt",
            "messages": [
                {"role": "user",
                 "content": {"type": "text",
                             "text": f"review this {lang} code:\n{code}"}},
                {"role": "assistant",
                 "content": {"type": "text", "text": "ready to review"}},
                {"role": "user",
                 "content": {"type": "image", "data": "aGk=", "mimeType": "image/png"}},
            ],
        }, None
    return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep stderr quiet for tests
        pass

    def _write(self, status, ctype, body):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _respond(self, rid, value=None, err=None, sse=False, session=None,
                 notifications=()):
        payload = {"jsonrpc": "2.0", "id": rid}
        if err is not None:
            payload["error"] = err
        else:
            payload["result"] = value
        text = json.dumps(payload)
        if session:
            self.send_response(200)
            self.send_header("Mcp-Session-Id", session)
        else:
            self.send_response(200)
        if sse:
            self.send_header("Content-Type", "text/event-stream")
            events = ["event: message\ndata: " + json.dumps(n) + "\n\n"
                      for n in notifications]
            events.append("event: message\ndata: " + text + "\n\n")
            body = "".join(events).encode()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self._write_after_headers("application/json", text.encode())

    def _write_after_headers(self, ctype, body):
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # Rich mode: standing SSE stream per session (the SDK opens one
        # after notifications/initialized). Comment heartbeats keep it
        # alive; DELETE (or process death) ends it. Default mode: 405,
        # which the SDK tolerates (on_error ignored, bounded retries).
        session = self.headers.get("Mcp-Session-Id")
        if not RICH or session not in SESSIONS:
            self._write(405, "text/plain", b"GET not supported")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        try:
            while session in SESSIONS:
                self.wfile.write(b": ping\n\n")
                self.wfile.flush()
                time.sleep(0.2)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        try:
            message = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, ValueError):
            self._write(400, "text/plain", b"invalid json")
            return
        method = message.get("method")
        rid = message.get("id")
        params = message.get("params") or {}
        session = self.headers.get("Mcp-Session-Id")

        if method == "initialize":
            new_session = uuid.uuid4().hex
            SESSIONS.add(new_session)
            capabilities = {"tools": {}}
            if RICH:
                capabilities["resources"] = {"listChanged": True}
                capabilities["prompts"] = {"listChanged": True}
            value = {
                "protocolVersion": params.get("protocolVersion", "2025-06-18"),
                "capabilities": capabilities,
                "serverInfo": {"name": "mock-mcp-http", "version": "0.1"},
            }
            # Streamable HTTP: respond via SSE to exercise that path.
            self._respond(rid, value=value, sse=True, session=new_session)
            return

        if session not in SESSIONS:
            self._write(404, "text/plain", b"unknown session")
            return

        if method == "notifications/initialized":
            self._write(202, "text/plain", b"")
            return
        if method == "tools/list":
            tools = [
                {"name": "echo", "description": "Echo text",
                 "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}},
                                 "required": ["text"]}},
                {"name": "fail", "description": "Always errors",
                 "inputSchema": {"type": "object"}},
            ]
            if RICH:
                tools.append({"name": "notify",
                              "description": "Emit list_changed notifications",
                              "inputSchema": {"type": "object"}})
            self._respond(rid, value={"tools": tools})
            return
        if method == "tools/call":
            name = params.get("name")
            args = params.get("arguments") or {}
            if RICH and name == "notify":
                # Piggybacked notifications ride the POST SSE response
                # ahead of the result (both backends dispatch them).
                self._respond(rid, value={
                    "content": [{"type": "text", "text": "notified"}],
                    "isError": False}, sse=True, notifications=[
                        {"jsonrpc": "2.0",
                         "method": "notifications/resources/list_changed"},
                        {"jsonrpc": "2.0",
                         "method": "notifications/prompts/list_changed"},
                    ])
                return
            if name in TOOL_RESULTS:
                text, is_error = TOOL_RESULTS[name](args)
                self._respond(rid, value={
                    "content": [{"type": "text", "text": text}], "isError": is_error})
            else:
                self._respond(rid, err={"code": -32602, "message": f"unknown tool {name}"})
            return
        if RICH and rid is not None:
            rich = rich_result(method, params)
            if rich is not None:
                value, err = rich
                self._respond(rid, value=value, err=err)
                return
        if rid is not None:
            self._respond(rid, err={"code": -32601, "message": f"no such method: {method}"})
        else:
            self._write(202, "text/plain", b"")

    def do_DELETE(self):
        session = self.headers.get("Mcp-Session-Id")
        SESSIONS.discard(session)
        self._write(200, "text/plain", b"")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 8933
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True  # standing GET streams must not block exit
    server.serve_forever()


if __name__ == "__main__":
    main()
