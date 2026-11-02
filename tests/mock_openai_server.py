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
                {"id": "mock-small", "object": "model"},
                {"id": "mock-large", "object": "model"},
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
        last_user = next(
            (m["content"] for m in reversed(messages) if m.get("role") == "user"), ""
        )
        if not self.headers.get("Authorization", "").startswith("Bearer "):
            self.send_error(401)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for word in ("echo:", f" {last_user[:40]}"):
            chunk = {"choices": [{"delta": {"content": word}}]}
            self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")

    def log_message(self, *args):
        pass  # keep stderr clean; never log request headers


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8931
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
