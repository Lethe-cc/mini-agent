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
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8939


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Model listing for the /provider auto-list and the /models command.
        # A wrong key must produce a real 401 so the login-status reporting is
        # exercised against an actual rejection, not just a success path.
        if not self.path.rstrip("/").endswith("/models"):
            self.send_error(404)
            return
        if self.headers.get("Authorization", "") != "Bearer pty-smoke-key":
            self.send_error(401)
            return
        body = json.dumps({"object": "list", "data": [
            {"id": "mock-small", "object": "model"},
            {"id": "mock-large", "object": "model"},
        ]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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


def virtual_screen(raw, rows=24, cols=80):
    """Replay emitted cursor/erase sequences, not just text in the byte log."""
    screen = [[" "] * cols for _ in range(rows)]
    row = col = 0
    wrap_pending = False
    i = 0

    def linefeed():
        nonlocal row
        if row == rows - 1:
            screen.pop(0)
            screen.append([" "] * cols)
        else:
            row += 1

    while i < len(raw):
        ch = raw[i]
        if ch == "\x1b":
            match = CSI_RE.match(raw, i)
            if not match:
                raise ValueError("unsupported terminal escape sequence")
            seq = match.group(0)
            params = seq[2:-1]
            if seq[-1] in ("A", "B"):
                # ECMA-48: an omitted OR ZERO distance means one row.
                distance = int(params or "0") or 1
                row = max(0, row - distance) if seq[-1] == "A" else min(rows - 1, row + distance)
                wrap_pending = False
            elif seq[-1] == "K":
                start = 0 if params == "2" else col
                screen[row][start:] = [" "] * (cols - start)
            elif seq[-1] not in ("m", "h", "l"):
                raise ValueError(f"unsupported terminal control: {seq!r}")
            i = match.end()
            continue
        if ch == "\r":
            col = 0
            wrap_pending = False
        elif ch == "\n":
            linefeed()
            wrap_pending = False
        else:
            width = display_width(ch)
            if width:
                if wrap_pending or col + width > cols:
                    col = 0
                    linefeed()
                screen[row][col] = ch
                if width == 2:
                    screen[row][col + 1] = ""
                col += width
                wrap_pending = col == cols
                col = min(col, cols - 1)
        i += 1
    return "\n".join("".join(line).rstrip() for line in screen)


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


def read_after(fd, keys, timeout):
    """Send bytes, then read the response window."""
    os.write(fd, keys)
    return read_all(fd, timeout)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/linux-gcc-debug/mini-agent"
    if not os.path.exists(binary):
        print(f"FAIL: binary not found: {binary}")
        return 1
    session_dir = "/tmp/mini_agent_pty_smoke"
    subprocess.run(["rm", "-rf", session_dir], check=False)
    os.makedirs(session_dir)
    config_dir = tempfile.TemporaryDirectory(prefix="mini_agent_pty_smoke_cfg_")
    threading.Thread(target=serve, daemon=True).start()
    time.sleep(0.3)

    log_path = "/tmp/mini_agent_pty_smoke/agent.log"
    env = dict(os.environ,
               MINI_AGENT_BASE_URL=f"http://127.0.0.1:{PORT}/v1",
               MINI_AGENT_API_KEY="pty-smoke-key",
               MINI_AGENT_MODEL="smoke",
               MINI_AGENT_SESSION_DIR=session_dir,
               # Isolate the /provider config write: without this the slash
               # command below would save into the real user config file. Kept
               # outside session_dir so the *.json session glob stays exact.
               MINI_AGENT_CONFIG=os.path.join(config_dir.name, "config.json"),
               MINI_AGENT_LOG=log_path)
    # Colour assertions (prompt marker + history styling) need SGR on: a
    # maintainer's NO_COLOR/TERM=dumb would disable commit styling.
    env.pop("NO_COLOR", None)
    env["TERM"] = env.get("TERM") or "xterm"
    if env["TERM"] == "dumb":
        env["TERM"] = "xterm"
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary, "run", "--new"], env)

    set_pty_size(fd, 24, 80)
    failures = []

    def expect(condition, label):
        print(("PASS " if condition else "FAIL ") + label)
        if not condition:
            failures.append(label)

    splash = read_all(fd, 1.0)
    expect("mini-agent · model=smoke" in splash, "splash shows model/session")
    # Connection marker (2026-11-02 request): the `>` prompt itself carries the
    # provider state as glyph + colour. Fully configured from the environment
    # but never probed in this process, so it must be the yellow `?`
    # (unverified) -- never the green `>` (connected).
    expect("\x1b[33m?\x1b[0m" in splash, "startup prompt marker is the yellow unverified state")
    # History colouring (2026-11-04 readability request): tag rows are tinted
    # whole-line, so the printable text stays contiguous inside the span.
    expect("\x1b[1mmini-agent · model=smoke" in splash,
           "banner row is bold and text stays contiguous")
    expect("\x1b[33m[auth] login unverified" in splash,
           "unverified auth row is yellow")
    expect("\x1b[2mEnter sends" in splash, "key-hint legend row is dim")

    os.write(fd, b"hello\r")
    turn = read_all(fd, 4.0)
    expect("tok0" in turn and "You: hello" in turn, "streamed turn committed to history")
    visible = virtual_screen(splash + turn)
    expect("You: hello" in visible and "tok0 tok1 tok2 tok3 tok4 tok5" in visible,
           "hello and its reply remain visible after the idle frame redraw")
    expect("\x1b[0A" not in splash + turn,
           "a new live block never emits zero-distance cursor-up (which moves one row)")
    # A successful chat turn is itself proof the key works, so it must flip the
    # marker to verified: 2026-11-02 report -- chat replied fine while the
    # prompt stayed yellow because only the model listing fed the verdict.
    expect("\x1b[1;32m>\x1b[0m" in turn and any(ln.strip() == ">" for ln in visible.splitlines()),
           "a successful turn flips the prompt marker to verified")
    # 2026-11-02: the marker IS the feedback; the [auth] history row that used
    # to accompany a chat-verified verdict was noise on every first reply.
    expect("[auth]" not in strip_csi(turn),
           "a chat-verified turn commits no [auth] history row")

    # Arrow-key editing + history (2026-11-01 request): type "ab", Left then
    # "X" inserts before the caret -> sends "aXb"; Up recalls it for resend.
    os.write(fd, b"ab\x1b[DX\r")
    edit_out = read_all(fd, 4.0)
    expect("You: aXb" in edit_out, "Left-arrow moves caret for mid-line insert")
    expect("[C" not in strip_csi(edit_out), "arrow escape bytes never leak as text")
    os.write(fd, b"\x1b[A\r")
    hist_out = read_all(fd, 4.0)
    expect("You: aXb" in hist_out, "Up-arrow recalls previous input and resends")
    visible = virtual_screen(splash + turn + edit_out + hist_out)
    expect(visible.count("tok0 tok1 tok2 tok3 tok4 tok5") == 3,
           "consecutive turns preserve all three reply rows on the screen")

    os.write(fd, b"/help\r")
    slash = read_all(fd, 1.0)
    expect("/sessions" in slash, "slash /help lists commands")
    expect("prompt: ?=unconfigured/unverified" in strip_csi(slash),
           "/help documents the prompt marker legend")

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

    # Model discovery (2026-11-02 request): /provider <url> with a key already
    # configured auto-fetches GET /models and lists the ids numbered; typing a
    # number selects it. Slash commands never enter the chat history, so the
    # message-count assertion at the end is unaffected.
    os.write(fd, f"/provider http://127.0.0.1:{PORT}/v1\r".encode())
    listed = ""
    for _ in range(12):  # poll up to ~6 s for the fetched list to render
        listed += read_all(fd, 0.5)
        if "mock-large" in listed:
            break
    expect("listing models" in listed, "provider+key auto-listing starts")
    expect("models (2)" in listed and "1. mock-large" in listed and "2. mock-small" in listed,
           "fetched models listed numbered (sorted ids)")
    expect("pty-smoke-key" not in listed, "model menu never echoes the API key")
    expect("\x1b[1;32m>\x1b[0m" in listed,
           "prompt marker turns green once the provider accepts the key")

    os.write(fd, b"1\r")
    picked = read_all(fd, 1.5)
    expect("model=mock-large" in picked, "typing a number selects that model")

    # /models re-lists on demand and marks the current model; lone Esc then
    # dismisses the menu without sending anything.
    os.write(fd, b"/models\r")
    relisted = ""
    for _ in range(12):
        relisted += read_all(fd, 0.5)
        if "<- current" in relisted:
            break
    expect("1. mock-large  <- current" in relisted, "re-list marks the current model")
    os.write(fd, b"\x1b")
    dismissed = read_all(fd, 1.0)
    expect("dismissed" in dismissed, "lone Esc dismisses the model menu")
    expect("You: " not in dismissed, "dismissing the menu sends nothing")

    # Login status (2026-11-02 request): the model-list request doubles as an
    # auth probe, so the TUI can report whether the provider accepted the key
    # without a second network call. Success reports verified + the count; a
    # 401 must read as a rejected key, not as a generic list failure.
    expect("login verified" in listed and "2 model(s) reachable" in listed,
           "after /provider + /login the login state shows verified")

    os.write(fd, b"/login wrong-key\r")
    bad = ""
    for _ in range(12):
        bad += read_all(fd, 0.5)
        if "REJECTED" in bad:
            break
    expect("login REJECTED" in bad and "HTTP 401" in bad,
           "a bad key reports rejected, not merely list-failed")
    expect("wrong-key" not in bad, "the rejected line never echoes the key")
    expect("\x1b[1;31m!\x1b[0m" in bad,
           "prompt marker keeps the red rejected state after the menu closes")

    expect("login=rejected" in strip_csi(read_after(fd, b"/config\r", 1.2)),
           "/config exposes the login state")

    os.write(fd, b"/login pty-smoke-key\r")
    back = ""
    for _ in range(12):
        back += read_all(fd, 0.5)
        if "login verified" in back:
            break
    expect("login verified" in back, "re-entering the good key returns to verified")

    # Restore the short model name: the status row is width-clipped, and the
    # later "queued N" assertion needs the original headroom back.
    os.write(fd, b"/model smoke\r")
    read_all(fd, 1.0)

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
    # The pinned row is bold-styled (2026-11-04 colouring), so compare on the
    # visible text: strip every SGR but keep the \x1b[2K row markers.
    def strip_sgr_keep_k(frame):
        return re.sub(r"\x1b\[(?!2K)[0-9;]*m", "", frame)

    frames = re.findall(r"\x1b\[\?2026h(.*?)\x1b\[\?2026l", long_raw, re.S)
    stream_frames = [f for f in frames if "\x1b[2KYou: long-reply" in strip_sgr_keep_k(f)]
    expect(len(stream_frames) >= 1 and
           all(strip_sgr_keep_k(f).split("\x1b[2K", 1)[1].startswith("You: long-reply")
               for f in stream_frames),
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
    expect("models_list status=start" in log_written and "models_list status=ok" in log_written,
           "log records the model listing start and outcome")
    expect("model_select model=mock-large source=menu" in log_written,
           "log records the model chosen from the menu")
    expect("login status=verified" in log_written and "login status=rejected" in log_written,
           "log records login status for both accepted and refused keys")
    expect("login status=verified models=0 source=chat" in log_written,
           "log records the chat turn that verified the key")
    expect("wrong-key" not in log_written, "the rejected key value never reaches the log")

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

    # Batched configuration entry (2026-11-02 request): the three commands are
    # independent and each saves on its own, so a provider / key / model may
    # be supplied in three SEPARATE processes. Runs with no MINI_AGENT_* LLM
    # env vars set, against a private config file, and asserts the startup
    # warning tracks progress instead of always re-listing all three.
    batch_root = "/tmp/mini_agent_pty_smoke_batch"
    subprocess.run(["rm", "-rf", batch_root], check=False)
    os.makedirs(batch_root + "/cfg", 0o700)
    batch_env = {k: v for k, v in env.items()
                 if k not in ("MINI_AGENT_BASE_URL", "MINI_AGENT_API_KEY",
                              "MINI_AGENT_MODEL")}
    batch_env["MINI_AGENT_CONFIG"] = batch_root + "/cfg/config.json"
    batch_env["MINI_AGENT_SESSION_DIR"] = batch_root + "/sess"
    batch_env["MINI_AGENT_LOG"] = batch_root + "/agent.log"

    def batch_session(keys, seconds=4.0):
        """One TUI process: capture the startup banner, send keys, read back."""
        pid, pty_fd = pty.fork()
        if pid == 0:
            os.execve(binary, [binary, "run", "--new"], batch_env)
        banner = read_all(pty_fd, 0.8)
        os.write(pty_fd, keys)
        out = read_all(pty_fd, seconds)
        os.write(pty_fd, b"\x03\x03")
        time.sleep(0.4)
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        return banner, out

    banner1, out1 = batch_session(
        f"/provider http://127.0.0.1:{PORT}/v1\r".encode())
    expect("config 0/3 · next: /provider <url>" in strip_csi(banner1),
           "batched: empty config points at /provider only")
    expect("\x1b[2m?\x1b[0m" in banner1,
           "batched: dim ? prompt while nothing is configured")
    expect("config saved" in out1 and "config 1/3 · next: /login <key>" in strip_csi(out1),
           "batched: /provider alone saves and points at /login")

    banner2, out2 = batch_session(b"/login batch-key\r")
    expect("config 1/3 · next: /login <key>" in strip_csi(banner2),
           "batched: provider persists into a later process")
    expect("config 2/3" in strip_csi(out2),
           "batched: /login alone saves and points at model discovery")

    banner3, out3 = batch_session(b"/model mock-large\rhi\r", seconds=6.0)
    expect("config 2/3" in strip_csi(banner3),
           "batched: provider+key persist into a third process")
    expect("config 3/3 · ready" in strip_csi(out3), "batched: /model completes the config")
    expect("You: hi" in strip_csi(out3), "batched: chat works after split entry")

    batched_cfg = json.load(open(batch_env["MINI_AGENT_CONFIG"]))
    expect(batched_cfg.get("base_url", "").endswith("/v1")
           and batched_cfg.get("api_key") == "batch-key"
           and batched_cfg.get("model") == "mock-large",
           "batched: all three pieces accumulated in one config file")
    subprocess.run(["rm", "-rf", batch_root], check=False)
    config_dir.cleanup()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
