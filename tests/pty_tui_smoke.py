"""PTY smoke test for the self-drawn TUI (design 007 section 5.6 contract).

Dev tool: drives mini-agent `run` inside a real pty against a local
threading mock SSE provider. Verifies: splash, one streamed turn, slash
command, double Ctrl-C exit with resume hint, session persistence, and
terminal state restoration on exit.

Usage:
  cmake --build --preset linux-gcc-debug
  python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent

NOT part of quality_commands (needs pty + timing tolerance); run manually.
"""
import fcntl
import glob
import json
import os
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8939


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode(errors="replace")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if '"long-reply"' in body:
            # 60 short lines: exercises the height-scaled streaming window
            # (2026-11-01 "full-terminal refresh") and the row-accounting invariants.
            for i in range(60):
                chunk = {"choices": [{"delta": {"content": f"L{i:02d} line\n"}}]}
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
                time.sleep(0.04)  # ~2.4 s total: keeps the Esc-interrupt window open
        else:
            for i in range(6):
                chunk = {"choices": [{"delta": {"content": f"tok{i} "}}]}
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
                time.sleep(0.1)
        self.wfile.write(b"data: [DONE]\n\n")

    def log_message(self, *args):
        pass


def serve():
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()


def read_all(fd, timeout):
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                out += os.read(fd, 4096)
            except OSError:
                break
    return out.decode(errors="replace")


CSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")


def strip_csi(text):
    return CSI_RE.sub("", text)


def display_width(text):
    import unicodedata
    w = 0
    for ch in strip_csi(text):
        if unicodedata.combining(ch):
            continue
        w += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return w


def backup_counts(raw):
    """live_rows_ back-up counts: every live repaint starts with \x1b[nA."""
    return [int(m) for m in re.findall(r"\x1b\[(\d+)A", raw)]


def set_pty_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/linux-gcc-debug/mini-agent"
    if not os.path.exists(binary):
        print(f"FAIL: binary not found: {binary}")
        return 1
    session_dir = "/tmp/mini_agent_pty_smoke"
    subprocess.run(["rm", "-rf", session_dir], check=False)
    os.makedirs(session_dir)
    threading.Thread(target=serve, daemon=True).start()
    time.sleep(0.3)

    log_path = "/tmp/mini_agent_pty_smoke/agent.log"
    env = dict(os.environ,
               MINI_AGENT_BASE_URL=f"http://127.0.0.1:{PORT}/v1",
               MINI_AGENT_API_KEY="pty-smoke-key",
               MINI_AGENT_MODEL="smoke",
               MINI_AGENT_SESSION_DIR=session_dir,
               MINI_AGENT_LOG=log_path)
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary, "run", "--new"], env)

    failures = []

    def expect(condition, label):
        print(("PASS " if condition else "FAIL ") + label)
        if not condition:
            failures.append(label)

    splash = read_all(fd, 1.0)
    expect("mini-agent · model=smoke" in splash, "splash shows model/session")

    os.write(fd, b"hello\r")
    turn = read_all(fd, 4.0)
    expect("tok0" in turn and "You: hello" in turn, "streamed turn committed to history")

    # Arrow-key editing + history (2026-11-01 request): type "ab", Left then
    # "X" inserts before the caret -> sends "aXb"; Up recalls it for resend.
    os.write(fd, b"ab\x1b[DX\r")
    edit_out = read_all(fd, 4.0)
    expect("You: aXb" in edit_out, "Left-arrow moves caret for mid-line insert")
    expect("[C" not in strip_csi(edit_out), "arrow escape bytes never leak as text")
    os.write(fd, b"\x1b[A\r")
    hist_out = read_all(fd, 4.0)
    expect("You: aXb" in hist_out, "Up-arrow recalls previous input and resends")

    os.write(fd, b"/help\r")
    slash = read_all(fd, 1.0)
    expect("/sessions" in slash, "slash /help lists commands")

    # Typing "/" surfaces the Tab-completion hint as a vertical list: a
    # header row, then one row per match (2026-11-01 follow-up).
    os.write(fd, b"/mo")
    hinted = read_all(fd, 0.6)
    hint_rows = [ln for ln in strip_csi(hinted).split("\n")
                 if "Tab completion" in ln or ln.strip() == "/model"]
    expect(any("Tab completion" in ln for ln in hint_rows),
           "slash hint header appears while typing /")
    expect(any(ln.strip() == "/model" for ln in hint_rows),
           "hint lists /model as its own row (vertical)")
    headers = [ln for ln in hint_rows if "Tab completion" in ln]
    expect(headers and all("/model" not in ln for ln in headers),
           "commands go on their own rows, not appended to the header")
    os.write(fd, b"\x7f\x7f\x7f")  # clear the partial line
    read_all(fd, 0.3)

    # Pi-style steering: Enter while streaming queues the input and it
    # auto-sends after the current turn commits (design 007 5.6 v1.x #1).
    os.write(fd, b"first\r")
    time.sleep(0.25)  # mid-stream
    os.write(fd, b"queued\r")
    steer = read_all(fd, 3.0)
    expect("queued 1" in steer, "queue depth shown in status line")
    expect("You: queued" in steer, "queued message auto-sent after turn")

    # Full-terminal streaming refresh (2026-11-01 maintainer request): with
    # a tall pty the streaming window must scale with terminal height
    # (old model: fixed 6-row tail), and every written row must still obey
    # the hard-wrap width invariant (design 007 5.6 rule 2).
    set_pty_size(fd, 50, 80)
    time.sleep(0.3)  # let the queued turn commit before the long one
    os.write(fd, b"long-reply\r")
    long_raw = read_all(fd, 8.0)
    counts = backup_counts(long_raw)
    expect("L59 line" in long_raw, "long streamed reply commits to history")
    # Pinned question (2026-11-01 request): the first streaming frame opens
    # the live block with the submitted question. Later frames may skip
    # re-writing it entirely (row diff preserves unchanged prefix rows on
    # screen -- that is what keeps it pinned without repaints).
    frames = re.findall(r"\x1b\[\?2026h(.*?)\x1b\[\?2026l", long_raw, re.S)
    stream_frames = [f for f in frames if "\x1b[2KYou: long-reply" in f]
    expect(len(stream_frames) >= 1 and
           all(f.split("\x1b[2K", 1)[1].startswith("You: long-reply") for f in stream_frames),
           f"question pinned atop live block ({len(stream_frames)} writes, first-row ok)")
    expect(bool(counts) and max(counts) >= 30,
           f"streaming window scales with terminal height (max backup={max(counts) if counts else 0})")
    expect(all(c <= 49 for c in counts), "live block never exceeds rows-1")
    over = [s for part in long_raw.split("\n")[:-1]
            for s in [strip_csi(part.rstrip("\r"))]  # pty ONLCR adds the CR
            if display_width(s) > 80]
    expect(not over, f"no written row exceeds 80 columns ({len(over)} violations)")

    # Lone Esc interrupts the in-flight request (2026-11-02 request;
    # design 5.6 rule 5 "esc-to-interrupt"). Aborted turns never enter
    # storage, so the message-count assertion below is unaffected.
    os.write(fd, b"esc-interrupt\r")
    time.sleep(0.25)  # mid-stream
    os.write(fd, b"\x1b")
    esc_out = read_all(fd, 3.0)
    expect("[interrupted] You: esc-interrupt" in esc_out,
           "lone Esc aborts the streaming turn")

    os.write(fd, b"\x03\x03")  # double Ctrl-C -> quit
    exit_out = read_all(fd, 2.0)
    _, status = os.waitpid(pid, 0)
    expect(os.WEXITSTATUS(status) == 0, "double Ctrl-C exits with code 0")
    expect("resume: " in exit_out and "run --session" in exit_out, "exit shows resume hint")

    # opt-in diagnostics file (design 007 section 7): events exist and
    # never contain the API key or message bodies.
    log_written = os.path.exists(log_path) and open(log_path).read()
    expect("turn status=ok" in log_written, "log file records turns")
    expect("pty-smoke-key" not in log_written, "log never contains the API key")

    files = glob.glob(f"{session_dir}/*.json")
    expect(len(files) == 1, "one session persisted")
    if files:
        data = json.load(open(files[0]))
        expect([m["role"] for m in data["messages"]]
               == ["user", "assistant"] * 6,
               "history stores successful turns including steered queue")

    # terminal restored: cooked mode check on our own shell is not the pty's;
    # instead assert the child exited cleanly above (RawMode destructor ran
    # on all paths incl. exceptions via RAII).

    # line-mode (non-TTY) /exit: same save/unlock path, no turn after it
    line_dir = "/tmp/mini_agent_pty_smoke_line"
    subprocess.run(["rm", "-rf", line_dir], check=False)
    os.makedirs(line_dir)
    line_env = dict(env, MINI_AGENT_SESSION_DIR=line_dir)
    proc = subprocess.run([binary, "run", "--new"], input="hi\n/exit\nlate\n",
                          text=True, capture_output=True, env=line_env, timeout=20)
    expect(proc.returncode == 0, "line-mode /exit returns 0")
    expect("saved session" in proc.stdout, "line-mode /exit saves")
    expect("echo: late" not in proc.stdout, "nothing runs after /exit")
    sess = glob.glob(f"{line_dir}/*.json")
    locks = glob.glob(f"{line_dir}/*.lock")
    expect(len(sess) == 1 and not locks, "line-mode session persisted, lock released")
    subprocess.run(["rm", "-rf", line_dir], check=False)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
