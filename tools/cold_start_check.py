"""Cold-start timing gate (project-context success criterion #1).

Dev tool: measures process-start-to-ready latency of the mini-agent
binary and asserts the 100 ms budget (network round trips excluded by
construction -- the ask probe talks to an instant loopback mock).

Two readings, N runs each (median is the gate, max is reported):
  1. `--version`: pure startup proxy (no network, no key, no config).
  2. piped `ask` against an instant local SSE mock: the full ready path
     (config merge + session setup + first request accepted). The mock
     answers immediately, so wall time ~= startup + loopback RTT.

Usage:
  python3 tools/cold_start_check.py ./build/linux-gcc-debug/mini-agent
  python3 tools/cold_start_check.py <binary> --runs 20 --budget-ms 100

Exit codes: 0 within budget, 1 over budget, 2 usage error.
NOT part of quality_commands; gated by the CI `smoke` job.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8943


class InstantHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        chunk = {"choices": [{"delta": {"content": "ok"},
                              "finish_reason": "stop"}]}
        self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")

    def log_message(self, *args):
        pass


def measure(cmd, env, runs):
    samples = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, env=env, capture_output=True, timeout=15)
        samples.append((time.perf_counter() - t0) * 1000)
        if r.returncode != 0:
            print(f"FAIL: command exited {r.returncode}: "
                  f"{r.stderr.decode(errors='replace')[:200]}")
            return None
    return samples


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("binary")
    ap.add_argument("--runs", type=int, default=15)
    ap.add_argument("--budget-ms", type=float, default=100.0)
    args = ap.parse_args()
    if not os.path.exists(args.binary):
        print(f"FAIL: binary not found: {args.binary}")
        return 2
    binary = os.path.abspath(args.binary)

    httpd = ThreadingHTTPServer(("127.0.0.1", PORT), InstantHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    time.sleep(0.2)

    tmp = tempfile.mkdtemp(prefix="mini-agent-coldstart-")
    env = dict(os.environ)
    env.pop("NO_COLOR", None)
    env.update({
        "MINI_AGENT_BASE_URL": f"http://127.0.0.1:{PORT}/v1",
        "MINI_AGENT_API_KEY": "coldstart-key",
        "MINI_AGENT_MODEL": "mock",
        "MINI_AGENT_CONFIG": os.path.join(tmp, "config.json"),
        "MINI_AGENT_SESSION_DIR": os.path.join(tmp, "sessions"),
        "MINI_AGENT_LOG": "off",
    })

    failures = 0
    for label, cmd in (
        ("--version (startup proxy)", [binary, "--version"]),
        ("ask (full ready path)", [binary, "ask", "ping"]),
    ):
        samples = measure(cmd, env, args.runs)
        if samples is None:
            failures += 1
            continue
        med, worst = statistics.median(samples), max(samples)
        ok = med <= args.budget_ms
        print(f"{'PASS' if ok else 'FAIL'} {label}: "
              f"median={med:.1f}ms max={worst:.1f}ms "
              f"(budget {args.budget_ms:.0f}ms, n={args.runs})")
        if not ok:
            failures += 1

    httpd.shutdown()
    subprocess.run(["rm", "-rf", tmp], check=False)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())