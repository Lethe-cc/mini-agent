"""PTY smoke test for the CLI SIGINT interrupt channel (v5, design 007 5.6).

Dev tool: drives mini-agent with a real controlling terminal against a
local threading mock SSE provider with a deliberately slow (trickle)
script, so a SIGINT lands mid-transfer. Verifies:
  1. one-shot ask on a pty: first Ctrl-C cancels cleanly ("cancelled:",
     exit 1, stream stops early); double Ctrl-C always terminates;
  2. line-mode (stdin pipe, stdout pty): SIGINT mid-turn cancels the turn
     and the session loop stays usable; idle SIGINT discards the input
     line without quitting; /exit still saves and exits 0;
  3. fully piped run: byte-identical script contract (no handler).

Usage:
  cmake --build --preset linux-gcc-debug
  python3 tests/pty_cli_smoke.py ./build/linux-gcc-debug/mini-agent

NOT part of quality_commands (needs pty + timing tolerance); run manually,
same convention as tests/pty_tui_smoke.py (both are gated by the CI
`smoke` job in .github/workflows/project.yml).
"""
import fcntl
import json
import os
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8941
failures = 0


def check(cond, name):
    global failures
    if cond:
        print(f"PASS {name}")
    else:
        failures += 1
        print(f"FAIL {name}")


class Handler(BaseHTTPRequestHandler):
    """SSE scripts keyed on the last user message prefix. TRICKLE paces
    chunks ~0.4 s apart so a SIGINT sent ~1.4 s after the request lands
    inside the transfer; PLAIN answers immediately (timing-independent).
    014: FLAKY* prefixes fail the first N requests with a transient HTTP
    status (counters below prove the client's retry budget); MIDRST
    delivers one content chunk then RSTs the connection (a stream-started
    failure the client must NOT retry)."""

    FLAKY_COUNTS = {}
    MIDRST_COUNT = 0

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode(errors="replace")
        try:
            msgs = json.loads(body).get("messages", [])
        except Exception:
            msgs = []
        last_user = next((m.get("content", "") for m in reversed(msgs)
                          if m.get("role") == "user"), "")
        # 014: transient-failure scripts (before any 200 is sent).
        for prefix, fails, status in (("FLAKY429", 2, 429),
                                      ("FLAKY503", 1, 503),
                                      ("FLAKYOFF", 1, 429)):
            if last_user.startswith(prefix):
                n = Handler.FLAKY_COUNTS.get(prefix, 0) + 1
                Handler.FLAKY_COUNTS[prefix] = n
                if n <= fails:
                    resp = json.dumps({"error": {"message": "transient"}}).encode()
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    if status == 429:
                        self.send_header("Retry-After", "1")
                    self.send_header("Content-Length", str(len(resp)))
                    self.end_headers()
                    self.wfile.write(resp)
                    return
        if last_user.startswith("MIDRST"):
            Handler.MIDRST_COUNT += 1
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            chunk = {"choices": [{"delta": {"content": "partial"}}]}
            self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
            self.wfile.flush()
            # RST (SO_LINGER 0): the client already parsed a data payload,
            # so the retry gate must stay closed despite the network error.
            try:
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                           struct.pack("ii", 1, 0))
                self.connection.close()
            except OSError:
                pass
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        try:
            if last_user.startswith("TRICKLE"):
                for i in range(12):
                    chunk = {"choices": [{"delta": {"content": f"w{i} "}}]}
                    self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                    self.wfile.flush()
                    time.sleep(0.4)
            elif last_user.startswith("SILENT"):
                # Fully silent stream (thinking-model emulation): headers
                # flushed, zero bytes for 6 s. Only the periodic XFERINFO
                # progress hook -- not the write callback -- can observe an
                # interrupt here.
                time.sleep(6)
                self.wfile.write(b'data: {"choices":[{"delta":{"content":"late"}}]}\n\n')
            else:
                for part in ("echo:", " " + last_user[:20]):
                    chunk = {"choices": [{"delta": {"content": part}}]}
                    self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
            self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\n')
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass  # client aborted mid-stream: expected in the cancel cases

    def log_message(self, *args):
        pass


def serve():
    httpd = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def make_env(session_dir):
    base = dict(os.environ)
    base.update({
        "MINI_AGENT_BASE_URL": f"http://127.0.0.1:{PORT}/v1",
        "MINI_AGENT_API_KEY": "smoke-key",
        "MINI_AGENT_MODEL": "smoke",
        "MINI_AGENT_SESSION_DIR": session_dir,
        "TERM": "xterm",
    })
    return base


class PtyProc:
    """Child attached to a pty (real controlling terminal). stdin_pipe
    True keeps stdin a pipe (line-mode: tty()==false) while stdout stays
    the pty, so the install gate (stdin OR stdout tty) holds and SIGINT
    can be sent directly to the process."""

    def __init__(self, binary, args, env, stdin_pipe=False):
        master, slave = pty.openpty()
        self.proc = subprocess.Popen(
            [binary] + args,
            stdin=subprocess.PIPE if stdin_pipe else slave,
            stdout=slave, stderr=slave, env=env)
        os.close(slave)
        self.master = master
        self.buf = b""

    def pump(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    data = b""
                if not data:
                    return
                self.buf += data

    def send_sigint(self):
        # Direct SIGINT: exercises the same handler path a tty Ctrl-C
        # would (the pty is here to satisfy the TTY install gate).
        # Tolerate a process that already exited (double-interrupt test:
        # the first cancel may finish before the second signal lands).
        try:
            self.proc.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass

    def feed(self, line: str):
        self.proc.stdin.write(line.encode())
        self.proc.stdin.flush()

    def alive(self):
        return self.proc.poll() is None

    def wait_exit(self, seconds=12):
        try:
            code = self.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
        # Drain the pty AFTER death: the child's last writes (cancelled:
        # error line, saved session banner) sit in the master buffer and
        # are otherwise lost.
        self.pump(0.6)
        return code

    def text(self):
        return self.buf.decode(errors="replace")

    def close(self):
        try:
            os.close(self.master)
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                             else "./build/linux-gcc-debug/mini-agent")
    check(os.path.exists(binary), f"binary exists: {binary}")
    if failures:
        return 1
    httpd = serve()
    time.sleep(0.2)
    session_dir = tempfile.mkdtemp(prefix="mini-agent-cli-smoke-")
    e = make_env(session_dir)

    # ---- 1. one-shot ask: mid-stream Ctrl-C cancels cleanly --------------
    p = PtyProc(binary, ["ask", "TRICKLE cancel me"], e)
    p.pump(1.4)  # ~3 chunks in; the request is streaming
    check(p.alive(), "ask: still streaming before interrupt")
    p.send_sigint()
    code = p.wait_exit(8)
    out = p.text()
    check("cancelled: request cancelled" in out, "ask: Ctrl-C prints cancelled error")
    check(code == 1, f"ask: cancel exits 1 (got {code})")
    check("w11" not in out, "ask: stream stopped early (no final chunks)")
    check(len(os.listdir(session_dir)) == 0, "ask: one-shot leaves no session file")
    p.close()

    # ---- 1b. SILENT stream: no bytes at all; only the XFERINFO progress
    # hook can observe the interrupt (write callback never fires) ---------
    p = PtyProc(binary, ["ask", "SILENT thinking model"], e)
    p.pump(2.0)  # headers sent, zero chunks: fully idle transfer
    check(p.alive(), "ask: idle transfer running before interrupt")
    p.send_sigint()
    code = p.wait_exit(8)
    out = p.text()
    check("cancelled: request cancelled" in out,
          "ask: Ctrl-C cancels an IDLE stream (XFERINFO checkpoint)")
    check(code == 1, f"ask: idle cancel exits 1 (got {code})")
    check("late" not in out, "ask: idle cancel stops before the late reply")
    p.close()

    # ---- 1c. double Ctrl-C always terminates (escape hatch) --------------
    p = PtyProc(binary, ["ask", "TRICKLE double"], e)
    p.pump(1.4)
    p.send_sigint()
    p.pump(0.5)
    p.send_sigint()  # second: SIG_DFL + raise -> signal death (exit -2)
    code = p.wait_exit(6)
    check(code in (-2, -9, 1), f"ask: double Ctrl-C terminates (exit {code})")
    p.close()

    # ---- 2. line-mode: mid-turn SIGINT cancels; loop stays usable ---------
    p = PtyProc(binary, ["run", "--new"], e, stdin_pipe=True)
    p.pump(1.5)
    check(b"session" in p.buf and b"type a line" in p.buf, "run: line-mode banner")
    p.feed("TRICKLE slow turn\n")
    p.pump(1.4)
    check(p.alive(), "run: turn streaming")
    p.send_sigint()
    p.pump(1.5)
    check("cancelled: request cancelled" in p.text(), "run: mid-turn SIGINT cancels")
    check(p.alive(), "run: session loop stays alive after cancel")
    p.feed("PLAIN next turn\n")
    p.pump(2.5)
    check("echo: PLAIN next turn" in p.text(), "run: turn after cancel works")

    # ---- 2b. idle SIGINT at the prompt: does not quit, loop keeps working -
    p.pump(0.5)  # drain the previous turn's output; now blocked in getline
    p.send_sigint()  # EINTR in getline -> clear + continue (flag == 1)
    p.pump(0.8)
    check(p.alive(), "run: idle SIGINT does not quit")
    p.feed("PLAIN idle-resume\n")
    p.pump(2.5)
    check("echo: PLAIN idle-resume" in p.text(),
          "run: turn works after an idle SIGINT")
    p.feed("/exit\n")
    code = p.wait_exit(8)
    check(code == 0, f"run: /exit exits 0 (got {code})")
    check("saved session" in p.text(), "run: /exit saves")
    files = [f for f in os.listdir(session_dir) if f.endswith(".json")]
    check(len(files) == 1, f"run: one session file persisted (got {len(files)})")
    p.close()

    # ---- 3. fully piped: byte-identical contract, no handler --------------
    r = subprocess.run([binary, "ask", "PLAIN piped"], env=e,
                       capture_output=True, text=True, timeout=20)
    check(r.returncode == 0 and r.stdout.strip() == "echo: PLAIN piped",
          "ask: piped run unaffected (stdout contract intact)")

    # ---- 4. 014 pre-stream retry: transient failures retried with backoff,
    # stream-started failures never retried, env knob disables -------------
    e4 = dict(e, MINI_AGENT_LOG=os.path.join(session_dir, "retry.log"))
    r = subprocess.run([binary, "ask", "FLAKY429 retry me"], env=e4,
                       capture_output=True, text=True, timeout=30)
    check(r.returncode == 0 and r.stdout.strip() == "echo: FLAKY429 retry me",
          "retry: 429x2 (Retry-After: 1) then success, full reply")
    check(Handler.FLAKY_COUNTS.get("FLAKY429") == 3,
          "retry: exactly 3 attempts for the 429x2 script")
    rlog = open(os.path.join(session_dir, "retry.log")).read()
    check(rlog.count("provider_retry") == 2 and "status=429" in rlog,
          "retry: two metadata-only provider_retry log events")
    check("retry me" not in rlog, "retry: log carries no message content")
    r = subprocess.run([binary, "ask", "FLAKY503 retry me"], env=e4,
                       capture_output=True, text=True, timeout=30)
    check(r.returncode == 0 and r.stdout.strip() == "echo: FLAKY503 retry me",
          "retry: 503x1 then success")
    check(Handler.FLAKY_COUNTS.get("FLAKY503") == 2,
          "retry: exactly 2 attempts for the 503x1 script")
    r = subprocess.run([binary, "ask", "MIDRST broken stream"], env=e4,
                       capture_output=True, text=True, timeout=30)
    check(r.returncode == 1, "retry: mid-stream RST fails with exit 1")
    check(Handler.MIDRST_COUNT == 1,
          "retry: no retry once a payload was delivered (stream-started gate)")
    e0 = dict(e4, MINI_AGENT_RETRIES="0")
    r = subprocess.run([binary, "ask", "FLAKYOFF no retry"], env=e0,
                       capture_output=True, text=True, timeout=30)
    check(r.returncode == 1 and Handler.FLAKY_COUNTS.get("FLAKYOFF") == 1,
          "retry: MINI_AGENT_RETRIES=0 restores fail-fast semantics")

    # ---- 5. 016 session export / prune -----------------------------------
    lc_dir = tempfile.mkdtemp(prefix="mini-agent-cli-lifecycle-")
    el = dict(e, MINI_AGENT_SESSION_DIR=lc_dir)

    def make_session(text):
        r = subprocess.run([binary, "run", "--new"], env=el,
                           input=f"{text}\n/exit\n", capture_output=True,
                           text=True, timeout=20)
        m = re.search(r"session ([0-9a-f]{16})", r.stdout)
        return m.group(1) if m else None

    id_a = make_session("EXPORTME alpha content")
    id_b = make_session("EXPORTME beta content")
    check(id_a is not None and id_b is not None and id_a != id_b,
          "lifecycle: two sessions created via line-mode")
    r = subprocess.run([binary, "session", "export", id_a], env=el,
                       capture_output=True, text=True, timeout=15)
    check(r.returncode == 0 and f"# session {id_a}" in r.stdout and
          "EXPORTME alpha content" in r.stdout and "## User" in r.stdout,
          "lifecycle: export md carries header and verbatim body")
    r = subprocess.run([binary, "session", "export", id_a, "--format", "json"],
                       env=el, capture_output=True, text=True, timeout=15)
    doc = json.loads(r.stdout) if r.returncode == 0 else {}
    check(r.returncode == 0 and doc.get("meta", {}).get("id") == id_a and
          any("EXPORTME alpha content" in m.get("content", "")
              for m in doc.get("messages", [])),
          "lifecycle: export json is the on-disk shape")
    out_md = os.path.join(lc_dir, "export.md")
    r = subprocess.run([binary, "session", "export", id_a, "--out", out_md],
                       env=el, capture_output=True, text=True, timeout=15)
    check(r.returncode == 0 and os.path.exists(out_md) and
          oct(os.stat(out_md).st_mode & 0o777) == "0o600",
          "lifecycle: export --out lands 0600")
    r = subprocess.run([binary, "session", "export", "ffffffffffffffff"], env=el,
                       capture_output=True, text=True, timeout=15)
    check(r.returncode == 1 and "error:" in r.stderr,
          "lifecycle: exporting an unknown id exits 1")
    # prune: explicit threshold required; dry-run lists; mtime gates deletion.
    r = subprocess.run([binary, "session", "prune"], env=el,
                       capture_output=True, text=True, timeout=15)
    check(r.returncode == 2 and "--older-than" in r.stderr,
          "lifecycle: prune without threshold exits 2")
    old = time.time() - 10 * 86400
    os.utime(os.path.join(lc_dir, id_a + ".json"), (old, old))
    r = subprocess.run([binary, "session", "prune", "--older-than", "5", "--dry-run"],
                       env=el, capture_output=True, text=True, timeout=15)
    check(r.returncode == 0 and f"would prune: {id_a}" in r.stdout and
          id_b not in r.stdout and
          os.path.exists(os.path.join(lc_dir, id_a + ".json")),
          "lifecycle: dry-run lists only the old session, deletes nothing")
    # A live lock protects even an old session.
    lock = os.path.join(lc_dir, id_a + ".json.lock")
    with open(lock, "w") as f:
        f.write(str(os.getpid()))
    r = subprocess.run([binary, "session", "prune", "--older-than", "5"],
                       env=el, capture_output=True, text=True, timeout=15)
    check(r.returncode == 0 and f"skipped (in use): {id_a}" in r.stdout and
          os.path.exists(os.path.join(lc_dir, id_a + ".json")),
          "lifecycle: locked session is skipped")
    os.remove(lock)
    r = subprocess.run([binary, "session", "prune", "--older-than", "5"],
                       env=el, capture_output=True, text=True, timeout=15)
    check(r.returncode == 0 and f"pruned: {id_a}" in r.stdout and
          not os.path.exists(os.path.join(lc_dir, id_a + ".json")) and
          os.path.exists(os.path.join(lc_dir, id_b + ".json")),
          "lifecycle: prune removes only the old unlocked session")
    subprocess.run(["rm", "-rf", lc_dir], check=False)

    # ---- 6. version identity: banners + version subcommand ----------------
    r = subprocess.run([binary, "version"], env=e, capture_output=True,
                       text=True, timeout=10)
    lines = r.stdout.splitlines()
    check(r.returncode == 0 and len(lines) == 3 and
          re.fullmatch(r"mini-agent \d+\.\d+\.\d+", lines[0]) is not None and
          lines[1].startswith("commit ") and len(lines[1]) > len("commit ") and
          lines[2].startswith("built ") and len(lines[2]) > len("built "),
          "version: prints version + commit + build time")
    r = subprocess.run([binary, "--version"], env=e, capture_output=True,
                       text=True, timeout=10)
    check(r.returncode == 0 and
          re.fullmatch(r"\d+\.\d+\.\d+", r.stdout.strip()) is not None,
          "version: --version stays version-only (script contract)")
    r = subprocess.run([binary, "run", "--new"], env=e,
                       input="/exit\n", capture_output=True, text=True, timeout=20)
    check("mini-agent v" in r.stdout and "session" in r.stdout,
          "version: line-mode banner carries the version")

    httpd.shutdown()
    subprocess.run(["rm", "-rf", session_dir], check=False)
    print(f"\n{'ALL PASS' if failures == 0 else str(failures) + ' FAILURE(S)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
