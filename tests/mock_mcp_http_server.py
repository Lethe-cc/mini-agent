#!/usr/bin/env python3
"""Stdlib-only mock MCP server over Streamable HTTP, for v2 HttpTransport tests.

Dev tool only; NOT a business quality gate. Implements one MCP endpoint that
supports POST (JSON-RPC messages) with both response shapes from the spec:
  * application/json for notifications (202 Accepted, no body)
  * text/event-stream carrying the JSON-RPC response as one SSE event
Session management: returns Mcp-Session-Id on initialize and requires it
afterwards (400 without, 404 for unknown ids so the client's re-init path
is exercised). Binds 127.0.0.1 only.

Usage:  python3 tests/mock_mcp_http_server.py 8933
"""
import json
import sys
import uuid
from http.server import BaseHTTPRequestHandler, HTTPServer

SSE_MODE = object()  # sentinel: this response ships as an SSE stream
TOOL_RESULTS = {
    "echo": lambda args: ("echoed: " + args.get("text", ""), False)
    if isinstance(args.get("text"), str) else ("missing string 'text'", True),
    "fail": lambda args: ("boom", True),
}
SESSIONS = set()


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

    def _respond(self, rid, value=None, err=None, sse=False, session=None):
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
            body = ("event: message\ndata: " + text + "\n\n").encode()
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
            value = {
                "protocolVersion": params.get("protocolVersion", "2025-06-18"),
                "capabilities": {"tools": {}},
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
            self._respond(rid, value={"tools": tools})
            return
        if method == "tools/call":
            name = params.get("name")
            args = params.get("arguments") or {}
            if name in TOOL_RESULTS:
                text, is_error = TOOL_RESULTS[name](args)
                self._respond(rid, value={
                    "content": [{"type": "text", "text": text}], "isError": is_error})
            else:
                self._respond(rid, err={"code": -32602, "message": f"unknown tool {name}"})
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
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8933
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
