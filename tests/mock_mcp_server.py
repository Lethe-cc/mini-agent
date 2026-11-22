#!/usr/bin/env python3
"""Stdlib-only mock MCP server over stdio, for mini-agent v2 tests.

Dev tool only; NOT a business quality gate. Speaks MCP JSON-RPC 2.0 with
newline-delimited framing (spec 2025-03-26/2025-06-18 stdio transport):
implements initialize / notifications/initialized / tools/list / tools/call
with two tools: echo(text) and cat_text_file(path, /tmp only).

--rich adds the M2a capability surface (design 022): resources + prompts
capabilities (listChanged), two-page resources/list, resources/read,
prompts/list + prompts/get, and a `notify` tool that emits both
notifications/resources/list_changed and notifications/prompts/list_changed
before its result (cache-invalidation E2E). Default mode output is
byte-identical to the pre-M2a mock.

Run directly for manual checks:
  python3 tests/mock_mcp_server.py [--rich]
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
NOTIFY_TOOL = {
    "name": "notify",
    "description": "Emit resources/prompts list_changed notifications.",
    "inputSchema": {"type": "object"},
}
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


def send(payload):
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


def result(rid, value):
    send({"jsonrpc": "2.0", "id": rid, "result": value})


def error(rid, code, message):
    send({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})


def call_tool(params, rich):
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
    if rich and name == "notify":
        # Piggybacked notifications BEFORE the result: both the legacy
        # recv loop and the SDK bridge dispatcher must surface them.
        send({"jsonrpc": "2.0", "method": "notifications/resources/list_changed"})
        send({"jsonrpc": "2.0", "method": "notifications/prompts/list_changed"})
        return {"content": [{"type": "text", "text": "notified"}], "isError": False}
    return {"content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}


def handle_rich(method, rid, params):
    """Rich-mode methods; returns True when handled."""
    if method == "resources/list":
        cursor = params.get("cursor")
        if cursor == "page2":
            result(rid, {"resources": RESOURCES[2:]})
        else:
            result(rid, {"resources": RESOURCES[:2], "nextCursor": "page2"})
        return True
    if method == "resources/read":
        uri = params.get("uri")
        if uri in RESOURCE_TEXTS:
            result(rid, {"contents": [{"uri": uri, "mimeType": "text/plain",
                                       "text": RESOURCE_TEXTS[uri]}]})
        else:
            error(rid, -32602, f"no such resource: {uri}")
        return True
    if method == "prompts/list":
        result(rid, {"prompts": PROMPTS})
        return True
    if method == "prompts/get":
        if params.get("name") != "review-code":
            error(rid, -32602, f"no such prompt: {params.get('name')}")
            return True
        args = params.get("arguments") or {}
        code = args.get("code", "")
        lang = args.get("lang", "")
        result(rid, {
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
        })
        return True
    return False


def main():
    rich = "--rich" in sys.argv[1:]
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
        params = message.get("params") or {}
        if method == "initialize":
            requested = params.get("protocolVersion")
            version = requested if requested in ("2025-06-18", "2025-03-26") else PROTOCOL_FALLBACK
            capabilities = {"tools": {"listChanged": False}}
            if rich:
                capabilities["resources"] = {"listChanged": True}
                capabilities["prompts"] = {"listChanged": True}
            result(rid, {
                "protocolVersion": version,
                "capabilities": capabilities,
                "serverInfo": {"name": "mock-mcp", "version": "0.1"},
            })
        elif method == "notifications/initialized":
            pass  # notifications get no response
        elif method == "tools/list":
            result(rid, {"tools": TOOLS + ([NOTIFY_TOOL] if rich else [])})
        elif method == "tools/call":
            result(rid, call_tool(params, rich))
        elif rich and rid is not None and handle_rich(method, rid, params):
            pass
        elif method == "ping":
            result(rid, {})
        elif rid is not None:
            error(rid, -32601, f"method not found: {method}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
