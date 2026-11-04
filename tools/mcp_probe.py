#!/usr/bin/env python3
"""Stdlib-only MCP client probe over Streamable HTTP.

Dev tool (see tools/README.md); NOT a business quality gate. Manually
exercise/debug any Streamable HTTP MCP server the way mini-agent's
HttpTransport does: same header rules (Mcp-Session-Id, MCP-Protocol-Version,
Accept: application/json, text/event-stream) and both response shapes
(JSON body or a single SSE "message" event).

Subcommands:
  probe      initialize -> notifications/initialized -> tools/list -> optional tools/call
  tools      handshake, then print tools/list only
  call       handshake, then tools/call one tool with JSON arguments
  request    handshake, then send one raw JSON-RPC method (params optional)
  raw        send one raw JSON-RPC message with NO handshake (protocol probing)

Add --raw to echo every JSON-RPC message as-is (>> sent, << received) on
stderr; the pretty summaries still go to stdout, so use `2>out.log` or
`2>&1 | jq` / `less` to inspect the wire data.

Examples (against the repo's mock server):
  python3 tests/mock_mcp_http_server.py 8933 &
  python3 tools/mcp_probe.py probe http://127.0.0.1:8933/ --call echo --args '{"text":"hi"}'
  python3 tools/mcp_probe.py call  http://127.0.0.1:8933/ fail
  python3 tools/mcp_probe.py raw   http://127.0.0.1:8933/ bogus/method   # expect -32601

Bearer token: --bearer or environment MCP_PROBE_BEARER (never echoed back).
Add --close to end the session with DELETE when the server supports it.
Exit code: 0 ok, 1 protocol/transport failure, 2 bad usage.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

PROTOCOL_VERSION = "2025-06-18"
CLIENT_INFO = {"name": "mcp-probe", "version": "0.1"}


def die(code, message):
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(code)


class HttpMcpClient:
    """Minimal Streamable HTTP MCP client: one endpoint, serial requests."""

    def __init__(self, url, bearer=None, timeout=15.0, protocol=PROTOCOL_VERSION,
                 verbose=False):
        self.url = url
        self.bearer = bearer
        self.timeout = timeout
        self.protocol = protocol
        self.verbose = verbose  # --raw: echo every JSON-RPC message on stderr
        self.session_id = None    # set from initialize response header
        self.negotiated = None    # server's protocolVersion after handshake
        self.next_id = 1

    def _open(self, request):
        try:
            return urllib.request.urlopen(request, timeout=self.timeout)
        except urllib.error.HTTPError as exc:
            return exc  # error responses still carry headers/body

    def _headers(self):
        headers = {
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        # 2025-06-18 MUST: every request after the negotiated version is known.
        if self.negotiated:
            headers["MCP-Protocol-Version"] = self.negotiated
        if self.bearer:
            headers["Authorization"] = f"Bearer {self.bearer}"
        return headers

    def _parse_body(self, resp, raw_bytes):
        """Return the JSON-RPC message, or None for empty/SSE-without-message."""
        ctype = (resp.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if resp.status == 202 or not raw_bytes:
            return None
        if ctype == "text/event-stream":
            # Spec: JSON-RPC payload arrives in data: lines of an event whose
            # type is "message" (default type also counts). Concatenate data
            # lines of the first message event.
            data, event = [], None
            for line in raw_bytes.decode("utf-8", "replace").splitlines():
                if line.startswith("event:"):
                    event = line[6:].strip()
                elif line.startswith("data:"):
                    data.append(line[5:].lstrip())
                elif not line.strip() and data:
                    if event in (None, "message"):
                        return json.loads("\n".join(data))
                    data, event = [], None
            if data:
                return json.loads("\n".join(data))
            return None
        return json.loads(raw_bytes.decode("utf-8", "replace"))

    def post(self, message, is_notification=False):
        """Send one JSON-RPC message. Returns (response message or None,
        transport description). Records session/version from responses."""
        if self.verbose:
            print(">> " + json.dumps(message, ensure_ascii=False), file=sys.stderr)
        body = json.dumps(message).encode()
        req = urllib.request.Request(self.url, data=body, headers=self._headers(),
                                     method="POST")
        # Remember pre-handshake state so a failed initialize does not leak a
        # half-set session into later requests.
        prev_session, prev_negotiated = self.session_id, self.negotiated
        status = None
        try:
            with self._open(req) as resp:
                raw = resp.read()
                status = resp.status
                session_hdr = resp.headers.get("Mcp-Session-Id")
                if session_hdr:
                    self.session_id = session_hdr
                parsed = self._parse_body(resp, raw)
        except urllib.error.URLError as exc:
            self.session_id, self.negotiated = prev_session, prev_negotiated
            return None, f"transport error: {exc.reason}"
        except (json.JSONDecodeError, ValueError) as exc:
            # Notifications have no reply body, so judge them by status only.
            if is_notification and status not in (200, 202, 204):
                self.session_id, self.negotiated = prev_session, prev_negotiated
                return None, f"notification rejected: HTTP {status}"
            self.session_id, self.negotiated = prev_session, prev_negotiated
            return None, f"non-JSON body (HTTP {status}): {exc}"
        if is_notification and status not in (200, 202, 204):
            self.session_id, self.negotiated = prev_session, prev_negotiated
            return None, f"notification rejected: HTTP {status}"
        if self.verbose:
            if parsed is not None:
                print("<< " + json.dumps(parsed, ensure_ascii=False), file=sys.stderr)
            else:
                print(f"<< (no JSON-RPC body, HTTP {status}, "
                      f"{len(raw)} bytes)", file=sys.stderr)
        result = parsed.get("result") if isinstance(parsed, dict) else None
        if isinstance(result, dict) and result.get("protocolVersion"):
            self.negotiated = result["protocolVersion"]
        return parsed, f"HTTP {status}"

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            message["params"] = params
        parsed, transport = self.post(message)
        if parsed is None:
            die(1, f"{method}: no JSON-RPC response ({transport})")
        if parsed.get("id") != rid:
            die(1, f"{method}: response id mismatch: {parsed.get('id')!r} != {rid}")
        return parsed

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        _, transport = self.post(message, is_notification=True)
        if transport.startswith("notification rejected"):
            die(1, f"{method}: {transport}")

    def initialize(self):
        result = self.request("initialize", {
            "protocolVersion": self.protocol,
            "capabilities": {},
            "clientInfo": CLIENT_INFO,
        })
        if "error" in result:
            die(1, f"initialize failed: {format_error(result['error'])}")
        value = result["result"]
        self.negotiated = value.get("protocolVersion", self.protocol)
        self.notify("notifications/initialized")
        return value

    def close_session(self):
        if not self.session_id:
            return "no session to close"
        req = urllib.request.Request(self.url, headers=self._headers(), method="DELETE")
        try:
            with self._open(req) as resp:
                return f"DELETE -> HTTP {resp.status}"
        except urllib.error.URLError as exc:
            return f"DELETE failed: {exc.reason}"


def format_error(err):
    return f"JSON-RPC {err.get('code')}: {err.get('message')}" + (
        f" data={json.dumps(err.get('data'))}" if err.get("data") is not None else "")


def show_response(parsed):
    if "error" in parsed:
        print(f"  error:   {format_error(parsed['error'])}")
    else:
        print("  result:  " + json.dumps(parsed.get("result"), indent=2, ensure_ascii=False))


def run_handshake(client, args):
    info = client.initialize()
    if not args.quiet:
        print(f"initialize ok: protocol={client.negotiated} "
              f"session={'yes' if client.session_id else 'none'}")
        print("  serverInfo:  " + json.dumps(info.get("serverInfo"), ensure_ascii=False))
        print("  capabilities:" + json.dumps(info.get("capabilities"), ensure_ascii=False))
    return info


def cmd_probe(args):
    client = make_client(args)
    run_handshake(client, args)
    tools = client.request("tools/list", {})
    if "error" in tools:
        print("tools/list -> " + format_error(tools["error"]))
    else:
        print_tools(tools["result"].get("tools", []))
    if args.call is not None:
        call_tool(client, args.call, parse_args_json(args.args))
    finish(client, args)


def cmd_tools(args):
    client = make_client(args)
    run_handshake(client, args)
    result = client.request("tools/list", {})
    if "error" in result:
        die(1, "tools/list -> " + format_error(result["error"]))
    print_tools(result["result"].get("tools", []))
    finish(client, args)


def cmd_call(args):
    client = make_client(args)
    run_handshake(client, args)
    call_tool(client, args.tool, parse_args_json(args.args))
    finish(client, args)


def cmd_request(args):
    client = make_client(args)
    run_handshake(client, args)
    result = client.request(args.method, parse_optional_json(args.params))
    print(f"{args.method} response:")
    show_response(result)
    finish(client, args)


def cmd_raw(args):
    client = make_client(args)
    if args.notification:
        message = {"jsonrpc": "2.0", "method": args.method}
        params = parse_optional_json(args.params)
        if params is not None:
            message["params"] = params
        parsed, transport = client.post(message, is_notification=True)
        print(f"notification -> {transport}" + (", reply below" if parsed else ""))
        if parsed:
            show_response(parsed)
        return
    message = {"jsonrpc": "2.0", "id": 1, "method": args.method}
    params = parse_optional_json(args.params)
    if params is not None:
        message["params"] = params
    parsed, transport = client.post(message)
    print(f"request -> {transport}")
    if parsed is None:
        die(1, "no JSON-RPC response")
    show_response(parsed)


def call_tool(client, name, arguments):
    result = client.request("tools/call", {"name": name, "arguments": arguments})
    print(f"tools/call {name!r} response:")
    show_response(result)
    value = result.get("result") or {}
    if value.get("isError"):
        print("  (server marked the call as a tool-level error)")


def parse_args_json(text):
    if not text:
        return {}
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        die(2, f"--args is not valid JSON: {exc}")
    if not isinstance(value, dict):
        die(2, "--args must be a JSON object")
    return value


def parse_optional_json(text):
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        die(2, f"params are not valid JSON: {exc}")


def print_tools(tools):
    if not tools:
        print("tools: (none)")
        return
    print(f"tools ({len(tools)}):")
    for tool in tools:
        schema = tool.get("inputSchema") or {}
        required = schema.get("required") or []
        props = ", ".join(schema.get("properties", {}))
        print(f"  - {tool.get('name')}: {tool.get('description', '')}")
        print(f"    params: [{props}] required: {required}")


def make_client(args):
    bearer = args.bearer or os.environ.get("MCP_PROBE_BEARER") or None
    return HttpMcpClient(args.url, bearer=bearer, timeout=args.timeout,
                         protocol=args.protocol, verbose=getattr(args, "raw", False))


def finish(client, args):
    if args.close:
        print(client.close_session())


def is_local_host(url):
    host = urllib.parse.urlparse(url).hostname or ""
    return host in ("127.0.0.1", "localhost", "::1")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="HTTP timeout seconds (default 15)")
    parser.add_argument("--protocol", default=PROTOCOL_VERSION,
                        help="protocolVersion to request in initialize")
    parser.add_argument("--bearer", help="Bearer token (prefer env MCP_PROBE_BEARER)")
    parser.add_argument("--quiet", action="store_true", help="hide handshake summary")
    parser.add_argument("--raw", action="store_true",
                        help="echo every raw JSON-RPC message (>> out, << in) on stderr")
    parser.add_argument("--close", action="store_true",
                        help="send DELETE to terminate the session at the end")
    subs = parser.add_subparsers(dest="cmd", required=True)
    # Mirror the global flags onto subcommands so they also work after the
    # subcommand name (argparse only parses top-level flags before it).
    common = argparse.ArgumentParser(add_help=False)
    for flag, kwargs in [
        ("--timeout", {"type": float, "default": argparse.SUPPRESS}),
        ("--protocol", {"default": argparse.SUPPRESS}),
        ("--bearer", {"default": argparse.SUPPRESS}),
        ("--quiet", {"action": "store_true", "default": argparse.SUPPRESS}),
        ("--raw", {"action": "store_true", "default": argparse.SUPPRESS}),
        ("--close", {"action": "store_true", "default": argparse.SUPPRESS}),
    ]:
        common.add_argument(flag, **kwargs)

    p = subs.add_parser("probe", parents=[common],
                        help="handshake + tools/list + optional tools/call")
    p.add_argument("url")
    p.add_argument("--call", metavar="TOOL", help="also call this tool")
    p.add_argument("--args", default="", help="JSON object of tool arguments")
    p.set_defaults(func=cmd_probe)

    p = subs.add_parser("tools", parents=[common], help="handshake + tools/list")
    p.add_argument("url")
    p.set_defaults(func=cmd_tools)

    p = subs.add_parser("call", parents=[common], help="handshake + tools/call")
    p.add_argument("url")
    p.add_argument("tool")
    p.add_argument("--args", default="", help="JSON object of tool arguments")
    p.set_defaults(func=cmd_call)

    p = subs.add_parser("request", parents=[common],
                        help="handshake + one raw JSON-RPC request")
    p.add_argument("url")
    p.add_argument("method")
    p.add_argument("--params", default="", help="JSON params (object)")
    p.set_defaults(func=cmd_request)

    p = subs.add_parser("raw", parents=[common],
                        help="one raw JSON-RPC message, no handshake")
    p.add_argument("url")
    p.add_argument("method")
    p.add_argument("--params", default="", help="JSON params (object)")
    p.add_argument("--notification", action="store_true", help="send without id")
    p.set_defaults(func=cmd_raw)

    args = parser.parse_args()
    if not args.url.startswith(("http://", "https://")):
        die(2, "url must be http(s)://...")
    bearer = args.bearer or os.environ.get("MCP_PROBE_BEARER")
    if bearer and args.url.startswith("http://") and not is_local_host(args.url):
        print("WARNING: sending a bearer token over plain HTTP to a non-local "
              "server", file=sys.stderr)
    args.func(args)


if __name__ == "__main__":
    main()
