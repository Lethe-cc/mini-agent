#!/usr/bin/env python3
"""Stdlib-only mock MCP server over stdio, for mini-agent v2 tests.

Dev tool only; NOT a business quality gate. Speaks MCP JSON-RPC 2.0 with
newline-delimited framing (spec 2025-03-26/2025-06-18 stdio transport):
implements initialize / notifications/initialized / tools/list / tools/call
with two tools: echo(text) and cat_text_file(path, /tmp only).

Run directly for manual checks:
  python3 tests/mock_mcp_server.py
"""
import json
import sys

PROTOCOL_FALLBACK = "2025-03-26"
TOOLS = [
    {
        "name": "echo",
        "description": "Echo the given text back.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "cat_text_file",
        "description": "Read a UTF-8 text file under /tmp.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
        },
    },
]


def send(payload):
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


def result(rid, value):
    send({"jsonrpc": "2.0", "id": rid, "result": value})


def error(rid, code, message):
    send({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})


def call_tool(params):
    name = params.get("name")
    args = params.get("arguments") or {}
    if name == "echo":
        text = args.get("text")
        if not isinstance(text, str):
            return {"content": [{"type": "text", "text": "missing string 'text'"}],
                    "isError": True}
        return {"content": [{"type": "text", "text": text}], "isError": False}
    if name == "cat_text_file":
        path = args.get("path") or ""
        if not path.startswith("/tmp/"):
            return {"content": [{"type": "text", "text": "path must be under /tmp"}],
                    "isError": True}
        try:
            with open(path, "r", encoding="utf-8") as handle:
                return {"content": [{"type": "text", "text": handle.read()}],
                        "isError": False}
        except OSError as exc:
            return {"content": [{"type": "text", "text": str(exc)}], "isError": True}
    return {"content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            sys.stderr.write("mock-mcp: non-JSON line ignored\n")
            continue
        method = message.get("method")
        rid = message.get("id")
        if method == "initialize":
            requested = (message.get("params") or {}).get("protocolVersion")
            version = requested if requested in ("2025-06-18", "2025-03-26") else PROTOCOL_FALLBACK
            result(rid, {
                "protocolVersion": version,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "mock-mcp", "version": "0.1"},
            })
        elif method == "notifications/initialized":
            pass  # notifications get no response
        elif method == "tools/list":
            result(rid, {"tools": TOOLS})
        elif method == "tools/call":
            result(rid, call_tool(message.get("params") or {}))
        elif method == "ping":
            result(rid, {})
        elif rid is not None:
            error(rid, -32601, f"method not found: {method}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
