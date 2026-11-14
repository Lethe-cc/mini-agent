"""PTY smoke test for the self-drawn TUI (design 007 section 5.6 contract).

Dev tool: drives mini-agent `run` inside a real pty against a local
threading mock SSE provider. Verifies: splash, one streamed turn, slash
command, double Ctrl-C exit with resume hint, session persistence, and
terminal state restoration on exit.

Usage:
  cmake --build --preset linux-gcc-debug
  python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent

NOT part of quality_commands (needs pty + timing tolerance); run manually
or via the CI `smoke` job (.github/workflows/project.yml).
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
    # Class attribute so the v6 multi-provider section can run a second
    # instance demanding its OWN key (per-profile auth must be provable).
    KEY = "pty-smoke-key"

    def do_GET(self):
        # Model listing for the /provider auto-list and the /models command.
        # A wrong key must produce a real 401 so the login-status reporting is
        # exercised against an actual rejection, not just a success path.
        if not self.path.rstrip("/").endswith("/models"):
            self.send_error(404)
            return
        if self.headers.get("Authorization", "") != f"Bearer {self.KEY}":
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
        try:
            req_msgs = json.loads(body).get("messages", [])
        except Exception:
            req_msgs = []
        # tool results of the CURRENT turn only: everything after the last
        # user message (a second EDITFLOW turn must not inherit round 1's
        # tool history).
        last_user_idx = max((i for i, m in enumerate(req_msgs)
                             if m.get("role") == "user"), default=-1)
        tool_rounds = sum(1 for m in req_msgs[last_user_idx + 1:]
                          if m.get("role") == "tool")
        last_user = next((m.get("content", "") for m in reversed(req_msgs)
                          if m.get("role") == "user"), "")
        # 015 (design 015): overflow-shaped 400 with a JSON error body
        # (not SSE) -- the client must surface the message and the hint.
        if last_user.startswith("OVERFLOW"):
            resp = json.dumps({"error": {
                "message": "This model's maximum context length is 8192 tokens. "
                           "However, your messages resulted in 9000 tokens."}}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        # 015: the /compact summary request (instruction prefix).
        if last_user.startswith("This is a context-compaction request"):
            for chunk in (
                {"choices": [{"delta": {"content": "COMPACT-SUMMARY: goals and files preserved"}}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ):
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
            self.wfile.write(b"data: [DONE]\n\n")
            return
        if last_user.startswith("EDITFLOW") and tool_rounds == 0:
            # v3 approval flow: request a workspace write; the client must
            # show the diff preview block and wait for y/n before running.
            frags = [
                {"choices": [{"delta": {"role": "assistant"}}]},
                {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "ap1",
                    "type": "function", "function": {"name": "write_file",
                    "arguments": ""}}]}}]},
                {"choices": [{"delta": {"tool_calls": [{"index": 0,
                    "function": {"arguments": '{"path":"note.txt"'}}]}}]},
                {"choices": [{"delta": {"tool_calls": [{"index": 0,
                    "function": {"arguments": ',"content":"hello from approval\\n"}'}}]}}]},
                {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
            ]
            for chunk in frags:
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
        elif last_user.startswith("EDITFLOW"):
            tool_text = next((m["content"] for m in reversed(req_msgs)
                              if m.get("role") == "tool"), "")
            for chunk in [
                {"choices": [{"delta": {"content": f"tool said: {tool_text[:40]}"}}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]:
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
        elif '"long-reply"' in body:
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


class BetaHandler(Handler):
    """Second provider endpoint for the v6 multi-provider section: same
    behavior, its OWN key, so a key accepted by alpha must not pass here."""

    KEY = "pty-beta-key"


BETA_PORT = 8941


def serve_beta():
    ThreadingHTTPServer(("127.0.0.1", BETA_PORT), BetaHandler).serve_forever()


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
    # v7 (design 013): env keeps winning on every derivation -- the pick is
    # saved into the auto-created default profile, but the effective model
    # stays the env one until the vars are unset and the process restarts.
    expect("model=default/smoke" in picked and "env overrides are active" in picked,
           "number pick saves the profile; env keeps the effective model")

    # /models re-lists on demand; lone Esc then dismisses the menu without
    # sending anything. Under env the effective model (smoke) is not in the
    # list, so no row carries the <- current marker.
    os.write(fd, b"/models\r")
    relisted = ""
    for _ in range(12):
        relisted += read_all(fd, 0.5)
        if "1. mock-large" in relisted:
            break
    expect("1. mock-large" in relisted and "<- current" not in relisted,
           "re-list renders the menu; env model outside the list is not marked")
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

    # v7 (design 013): under env override /login saves the profile key, but
    # the effective key stays the env one on every derivation -- the verdict
    # remains verified. The rejected-key path is covered by the dedicated
    # no-env session below (and by multi's per-profile 401).
    os.write(fd, b"/login wrong-key\r")
    bad = ""
    for _ in range(12):
        bad += read_all(fd, 0.5)
        if "login verified" in bad:
            break
    expect("wrong-key" not in bad, "the saved key value never echoes")
    expect("login verified" in bad and "login REJECTED" not in bad,
           "env keeps the effective key: a profile-only edit cannot reject it")

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
    # v4.11 turn panel (design 010 §21): while a turn streams, the live
    # region carries the process view -- at minimum the "this turn Ns"
    # header row (tools/thinking rows appear only when a turn uses them).
    expect("this turn" in strip_csi(long_raw), "turn panel header shown while streaming")
    # v4.12: bottom system-info row (user@host · cwd · clock) present by
    # default; OSC-capable strips keep it visible in the cleaned stream.
    expect(re.search(r"@\S+ \u00b7 \S+ \u00b7 \d\d:\d\d:\d\d", strip_csi(splash + turn))
               is not None,
           "system info row shows user@host · cwd · clock")
    # v4.13 sidebar: /sidebar on composes a right column (tree rows joined
    # by the " | " gutter) inside the inline frame; width invariant holds.
    os.write(fd, b"/sidebar on\r")
    side_raw = ""
    for _ in range(20):
        side_raw += read_all(fd, 0.3)
        if "workspace" in strip_csi(side_raw) and "|" in strip_csi(side_raw):
            break
    side_clean = strip_csi(side_raw)
    side_rows = [l for l in side_clean.split("\r\n") if " | " in l]
    expect(len(side_rows) >= 2, "sidebar composes tree rows with the gutter")
    expect(all(display_width(l.rstrip("\r")) <= 80 for l in side_rows),
           "sidebar composed rows respect the 80-col width invariant")
    # v4.14: Ctrl-F focuses the sidebar (reverse-video selection), arrows
    # walk selectable rows, Enter inserts the workspace-relative path.
    # Wait for the scan to land first: the panel shows "(scanning...)"
    # until the first result arrives (git status on a real repo takes a
    # moment).
    for _ in range(12):
        probe = strip_csi(read_all(fd, 0.5))
        if "workspace" in probe and "(scanning" not in probe:
            break
    os.write(fd, b"\x06")
    focus_raw = read_all(fd, 1.0)
    expect("\x1b[7m" in focus_raw, "sidebar focus highlights a row")
    expect("p=preview" in strip_csi(focus_raw),
           "v5 sidebar focus entry shows the key legend (p=preview)")
    os.write(fd, b"p")
    pvk = strip_csi(read_all(fd, 1.0))
    expect("[preview]" in pvk or "[error] preview" in pvk,
           "v5 `p` in sidebar focus previews the highlighted row")
    os.write(fd, b"\x1b[B\r")
    picked = read_all(fd, 1.0)
    expect("[sidebar] inserted:" in strip_csi(picked),
           "Enter on a focused sidebar row inserts the path")
    os.write(fd, b"\x08" * 40)  # clear the inserted path from the composer
    read_all(fd, 0.3)
    os.write(fd, b"/sidebar off\r")
    read_all(fd, 0.5)
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
    # v6 (design 012): outcome events carry provider=<name> before status=.
    expect("models_list status=start" in log_written
           and re.search(r"models_list .*status=ok", log_written),
           "log records the model listing start and outcome")
    expect("model_select model=smoke source=menu provider=default" in log_written,
           "log records the menu selection (effective env model, saved profile)")
    expect("login status=verified" in log_written,
           "log records the accepted-key login status")
    expect("login status=verified models=0 source=chat" in log_written,
           "log records the chat turn that verified the key")
    expect("wrong-key" not in log_written, "the rejected key value never reaches the log")

    files = glob.glob(f"{session_dir}/*.json")
    expect(len(files) == 1, "one session persisted")
    if files:
        data = json.load(open(files[0]))
        # v3: fresh sessions open with the injected system prompt, so the
        # turn history is [system] + N*(user, assistant) pairs.
        expect(data["messages"][0]["role"] == "system",
               "v3 session starts with the system prompt")
        expect([m["role"] for m in data["messages"][1:]]
               == ["user", "assistant"] * 6,
               "history stores successful turns including steered queue")

    # terminal restored: cooked mode check on our own shell is not the pty's;
    # instead assert the child exited cleanly above (RawMode destructor ran
    # on all paths incl. exceptions via RAII).

    # v3 TUI approval + diff preview (design 009 5.3): a fresh TUI process
    # with a toy workspace. write_file must raise the approval block with
    # the diff rows (+green), y allows, the tool row lands in history, and
    # the file is written. Then /cwd queries the boundary.
    ws_dir = "/tmp/mini_agent_pty_smoke_ws"
    subprocess.run(["rm", "-rf", ws_dir], check=False)
    os.makedirs(ws_dir)
    approval_dir = "/tmp/mini_agent_pty_smoke_appr"
    subprocess.run(["rm", "-rf", approval_dir], check=False)
    os.makedirs(approval_dir)
    appr_env = dict(env, MINI_AGENT_SESSION_DIR=approval_dir,
                    MINI_AGENT_LOG=approval_dir + "/agent.log",
                    # /approve persists the config (workspace included); keep
                    # it private to this scenario so later runs sharing the
                    # main config file never inherit a deleted workspace.
                    MINI_AGENT_CONFIG=os.path.join(approval_dir, "config.json"))
    apid, afd = pty.fork()
    if apid == 0:
        os.execve(binary, [binary, "run", "--new", "--cwd", ws_dir], appr_env)
    set_pty_size(afd, 24, 80)
    read_all(afd, 0.8)  # banner
    os.write(afd, b"EDITFLOW please\r")
    appr_out = read_all(afd, 5.0)
    plain = strip_csi(appr_out)
    expect("AWAITING APPROVAL" in plain or "ALLOW write_file" in plain,
           "v3 approval block appears for write_file")
    expect("\x1b[32m+hello from approval" in appr_out,
           "v3 diff preview renders added line in green (git default)")
    expect("\x1b[36m@@ " in appr_out,
           "v3 diff hunk header cyan (git fragheader default)")
    expect("\x1b[1m--- a/" in appr_out,
           "v3 diff file header bold (git meta default)")
    expect("+hello from approval" in plain and "-(no content)" not in plain,
           "v3 diff rows visible with content")
    expect("a=allow ALL (session)" in plain,
           "approval legend states the session-wide scope of `a`")
    os.write(afd, b"y")
    approved = read_all(afd, 5.0)
    approved_plain = strip_csi(approved)
    expect("[tool] write_file" in approved_plain and "ok" in approved_plain,
           "v3 approved tool row lands in history")
    # v5 (design 011): after execution the applied diff echoes into the
    # transcript (green added rows) right below the [tool] row -- the
    # edit -> diff -> effect flow, distinct from the pre-approval preview.
    expect("\x1b[32m+hello from approval" in approved,
           "v5 applied diff echoes in transcript after execute")
    expect("tool said: created" in approved_plain or "tool said: wrote" in approved_plain,
           "v3 mock consumed the tool result (flow completed)")
    expect(os.path.exists(ws_dir + "/note.txt")
           and "hello from approval" in open(ws_dir + "/note.txt").read(),
           "v3 approved write landed on disk inside the workspace")
    # Session-allow (`a`): the consent echo must name the argument-blind
    # scope, and the NEXT call of the same tool must skip approval.
    os.write(afd, b"EDITFLOW session allow\r")
    sa_block = strip_csi(read_all(afd, 5.0))
    expect("ALLOW write_file" in sa_block, "session-allow: approval block appears")
    os.write(afd, b"a")
    sa_echo = strip_csi(read_all(afd, 5.0))
    expect("session-allow: every write_file call runs unapproved this session" in sa_echo,
           "session-allow: consent echo names the scope")
    os.write(afd, b"EDITFLOW third\r")
    sa_next = strip_csi(read_all(afd, 6.0))
    expect("AWAITING APPROVAL" not in sa_next and "[tool] write_file" in sa_next,
           "session-allow: same-name call skips approval")
    os.write(afd, b"/cwd\r")
    cwd_out = read_all(afd, 2.5)
    expect("workspace: " in strip_csi(cwd_out),
           "v3 /cwd shows the session workspace")
    # v5 (design 011): /preview shows a workspace file to the human without
    # a model turn. note.txt is plain text (verbatim rows); escape the
    # boundary must refuse.
    os.write(afd, b"/preview note.txt\r")
    prev_out = strip_csi(read_all(afd, 2.0))
    expect("[preview] note.txt" in prev_out and "hello from approval" in prev_out,
           "v5 /preview shows the file body")
    os.write(afd, b"/preview ../../etc/passwd\r")
    prev_esc = strip_csi(read_all(afd, 2.0))
    expect("[error] preview" in prev_esc and "escapes workspace" in prev_esc,
           "v5 /preview refuses workspace escape")
    # v5 5.6: /preview of a recognized code file highlights (def -> cyan).
    open(ws_dir + "/main.py", "w").write("def greet():\n    return 1\n")
    os.write(afd, b"/preview main.py\r")
    py_out = read_all(afd, 2.0)
    expect("[preview] main.py" in strip_csi(py_out), "v5 /preview shows the code file")
    expect("\x1b[36mdef\x1b[0m" in py_out, "v5 code preview highlights keyword cyan")
    os.remove(ws_dir + "/main.py")
    # v3.1 runtime approval switch: query shows current policy; switching
    # to off persists + warns; the NEXT tool call then runs WITHOUT an
    # approval block (status row carries the auto-tools marker instead).
    os.write(afd, b"/approve\r")
    apq = read_all(afd, 1.5)
    expect("approval=ask" in strip_csi(apq), "/approve queries the current policy")
    os.write(afd, b"/approve off\r")
    apoff = read_all(afd, 1.5)
    plain_apoff = strip_csi(apoff)
    expect("tool_approval=off" in plain_apoff and "WITHOUT asking" in plain_apoff,
           "/approve off warns and switches")
    os.write(afd, b"EDITFLOW again\r")
    auto_run = read_all(afd, 6.0)
    plain_auto = strip_csi(auto_run)
    expect("AWAITING APPROVAL" not in plain_auto and "[tool] write_file" in plain_auto,
           "off policy: tool runs with no approval block")
    expect("auto-tools" in plain_auto, "status row shows the persistent auto-tools marker")
    expect(os.path.exists(ws_dir + "/note.txt"), "off-policy write landed")
    # Log red-line extension: file content must never reach the log.
    os.write(afd, b"\x03\x03")
    read_all(afd, 2.0)
    try:
        os.waitpid(apid, 0)
    except ChildProcessError:
        pass
    appr_log = open(approval_dir + "/agent.log").read() if os.path.exists(
        approval_dir + "/agent.log") else ""
    expect("hello from approval" not in appr_log,
           "v3 file content never reaches the log (metadata only)")
    expect("tool_approve" in appr_log and "write_file" in appr_log,
           "v3 tool_approve event recorded (metadata)")
    subprocess.run(["rm", "-rf", ws_dir], check=False)
    subprocess.run(["rm", "-rf", approval_dir], check=False)

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

    # v4.15 (maintainer request): bare `run` reuses the most recently updated
    # session instead of opening a new one each time; --temp opens an ephemeral
    # session that is never saved. Line mode keeps this deterministic.
    # The earlier line-mode run saved one session; `run --new` below adds a
    # second. Counts after that are asserted against the new baseline.
    proc = subprocess.run([binary, "run", "--new"], input="first\n/exit\n",
                          text=True, capture_output=True, env=line_env, timeout=20)
    m = re.search(r"session ([0-9a-f]{16})", proc.stdout)
    expect(m is not None, "v4.15 baseline run saved a session id")
    first_id = m.group(1) if m else ""
    baseline = len(glob.glob(f"{line_dir}/*.json"))
    expect(baseline == 2, "v4.15 baseline: two line-mode sessions on disk")
    proc2 = subprocess.run([binary, "run"], input="/exit\n",
                           text=True, capture_output=True, env=line_env, timeout=20)
    m2 = re.search(r"session ([0-9a-f]{16})", proc2.stdout)
    expect(m2 is not None and m2.group(1) == first_id,
           "v4.15 bare run reuses the latest session")
    expect(len(glob.glob(f"{line_dir}/*.json")) == baseline,
           "v4.15 reuse does not create a second session file")
    proc3 = subprocess.run([binary, "run", "--temp"], input="temp turn\n/exit\n",
                           text=True, capture_output=True, env=line_env, timeout=20)
    expect("discarded" in proc3.stdout and "not saved" in proc3.stdout,
           "v4.15 --temp exits with a discarded-session note")
    expect(len(glob.glob(f"{line_dir}/*.json")) == baseline,
           "v4.15 --temp never writes a session file")
    proc4 = subprocess.run([binary, "run", "--temp", "--session", "abcd"],
                           input="/exit\n", text=True, capture_output=True,
                           env=line_env, timeout=20)
    expect(proc4.returncode == 2 and "--temp cannot be combined" in proc4.stderr,
           "v4.15 --temp + --session rejected as config error")
    expect(not glob.glob(f"{line_dir}/*.lock"), "v4.15 reuse/temp runs release locks")
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

    # v4.9: bare `mini-agent` (no `run` subcommand) opens the TUI, and
    # /session switches sessions in place (save+lock swap, idle-gated).
    def bare_session(keys, seconds=4.0):
        pid, pty_fd = pty.fork()
        if pid == 0:
            os.execve(binary, [binary], batch_env)
        out = read_all(pty_fd, 0.8)
        os.write(pty_fd, keys)
        out += read_all(pty_fd, seconds)
        os.write(pty_fd, b"\x03\x03")
        time.sleep(0.4)
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        return out

    bare = bare_session(b"/session new\r")
    expect("mini-agent · model=" in strip_csi(bare),
           "bare: mini-agent with no subcommand opens the TUI")
    expect("[ok] new session" in strip_csi(bare),
           "bare: /session new forks a fresh session in place")

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
    # v7 (design 013): the three pieces accumulate in the auto-created
    # default profile; no top-level LLM fields exist anymore.
    batched_default = batched_cfg.get("providers", {}).get("default", {})
    expect(batched_default.get("base_url", "").endswith("/v1")
           and batched_default.get("api_key") == "batch-key"
           and batched_default.get("model") == "mock-large"
           and batched_cfg.get("active_provider") == "default"
           and "base_url" not in batched_cfg,
           "batched: all three pieces accumulated in the default profile")

    # v7 (design 013): the rejected-key verdict needs a process WITHOUT env
    # overrides (env keeps winning over profile edits). One short no-env
    # session: auto-default profile + a bad key -> 401 -> red marker, then
    # the good key restores verified.
    rej_root = "/tmp/mini_agent_pty_smoke_rej"
    subprocess.run(["rm", "-rf", rej_root], check=False)
    os.makedirs(rej_root + "/cfg", 0o700)
    rej_env = dict(batch_env,
                   MINI_AGENT_CONFIG=rej_root + "/cfg/config.json",
                   MINI_AGENT_SESSION_DIR=rej_root + "/sess",
                   MINI_AGENT_LOG=rej_root + "/agent.log")
    rpid, rfd = pty.fork()
    if rpid == 0:
        os.execve(binary, [binary, "run", "--new"], rej_env)
    set_pty_size(rfd, 24, 80)
    read_all(rfd, 0.8)
    os.write(rfd, f"/provider http://127.0.0.1:{PORT}/v1\r".encode())
    read_all(rfd, 0.8)
    os.write(rfd, b"/login wrong-key\r")
    bad = ""
    for _ in range(12):
        bad += read_all(rfd, 0.5)
        if "REJECTED" in bad:
            break
    expect("login REJECTED" in bad and "HTTP 401" in bad,
           "a bad key reports rejected, not merely list-failed")
    expect("wrong-key" not in bad, "the rejected line never echoes the key")
    expect("\x1b[1;31m!\x1b[0m" in bad,
           "prompt marker keeps the red rejected state after the menu closes")
    expect("login=rejected" in strip_csi(read_after(rfd, b"/config\r", 1.2)),
           "/config exposes the login state")
    os.write(rfd, b"/login pty-smoke-key\r")
    back = ""
    for _ in range(12):
        back += read_all(rfd, 0.5)
        if "login verified" in back:
            break
    expect("login verified" in back, "re-entering the good key returns to verified")
    os.write(rfd, b"\x03\x03")
    time.sleep(0.4)
    try:
        os.waitpid(rpid, 0)
    except ChildProcessError:
        pass
    os.close(rfd)
    rej_log = open(rej_env["MINI_AGENT_LOG"]).read()
    expect("login status=rejected" in rej_log and "wrong-key" not in rej_log,
           "rejected verdict logged without the key value")
    subprocess.run(["rm", "-rf", rej_root], check=False)

    # 015 (design 015 §6): context-overflow hint + manual /compact, in a
    # dedicated no-env session (own config/sessions/log). Two turns build
    # history; /compact replaces it with the mock summary; the next turn
    # must carry the summary; an OVERFLOW turn renders the 400 hint.
    cmp_root = "/tmp/mini_agent_pty_smoke_cmp"
    subprocess.run(["rm", "-rf", cmp_root], check=False)
    os.makedirs(cmp_root + "/cfg", 0o700)
    cmp_env = dict(batch_env,
                   MINI_AGENT_CONFIG=cmp_root + "/cfg/config.json",
                   MINI_AGENT_SESSION_DIR=cmp_root + "/sess",
                   MINI_AGENT_LOG=cmp_root + "/agent.log")
    cpid, cfd = pty.fork()
    if cpid == 0:
        os.execve(binary, [binary, "run", "--new"], cmp_env)
    set_pty_size(cfd, 24, 80)
    read_all(cfd, 0.8)

    def cmp_send(keys, marker, tries=16):
        os.write(cfd, keys)
        out = ""
        for _ in range(tries):
            out += read_all(cfd, 0.5)
            if marker in out:
                break
        return out

    early = cmp_send(b"/compact\r", "nothing to compact")
    expect("nothing to compact" in early, "compact: empty history is a no-op")
    # No-env session: configure the mock route first (auto-default profile).
    cmp_send(f"/provider http://127.0.0.1:{PORT}/v1\r".encode(), "models (2)")
    cmp_send(b"/login pty-smoke-key\r", "login verified")
    cmp_send(b"/model mock-small\r", "mock-small")
    cmp_send(b"first turn\r", "tok0")
    cmp_send(b"second turn\r", "tok0")
    compacted = cmp_send(b"/compact\r", "[ok] compacted")
    expect("[ok] compacted" in compacted and "messages into a summary" in compacted,
           "compact: history replaced by the summary")
    sess_files = glob.glob(cmp_root + "/sess/*.json")
    expect(len(sess_files) == 1, "compact: one session file")
    sess_doc = json.load(open(sess_files[0]))
    sess_msgs = sess_doc.get("messages", [])
    # The default system prompt survives; everything else is exactly the
    # marked summary (no transcript leftovers).
    expect(len(sess_msgs) == 2 and sess_msgs[0].get("role") == "system" and
           "[conversation summary]" in sess_msgs[1].get("content", "") and
           "COMPACT-SUMMARY" in sess_msgs[1].get("content", "") and
           all("first turn" not in m.get("content", "") and
               "second turn" not in m.get("content", "") for m in sess_msgs),
           "compact: persisted history is system prompt + marked summary only")
    after = cmp_send(b"after compact\r", "tok0")
    expect("You: after compact" in after, "compact: the next turn works")
    overflow = cmp_send(b"OVERFLOW too long\r", "[hint] context window exceeded", tries=20)
    expect("[hint] context window exceeded" in overflow and "/compact" in overflow,
           "overflow: 400 renders the actionable /compact hint")
    expect("maximum context length is 8192" in overflow,
           "overflow: the gateway error message is surfaced")
    os.write(cfd, b"\x03\x03")
    time.sleep(0.4)
    try:
        os.waitpid(cpid, 0)
    except ChildProcessError:
        pass
    os.close(cfd)
    # Read the log AFTER exit: spdlog flushes on shutdown.
    cmp_log = open(cmp_env["MINI_AGENT_LOG"]).read()
    expect("compact status=ok" in cmp_log and "first turn" not in cmp_log,
           "compact: metadata-only log event, no message content")
    subprocess.run(["rm", "-rf", cmp_root], check=False)

    # v6 multi-provider end-to-end (design 012 §8 M4): two mock endpoints
    # demanding DIFFERENT keys, one long-lived TUI process. Proves profiles
    # persist side by side, per-profile auth verdicts stay separate, the
    # aggregated menu qualifies ids, qualified /model switches atomically,
    # ambiguous bare names fail closed, and rm of the active profile keeps
    # the session usable. No MINI_AGENT_* LLM env vars (batch_env already
    # strips them), private config/session/log under multi_root.
    multi_root = "/tmp/mini_agent_pty_smoke_multi"
    subprocess.run(["rm", "-rf", multi_root], check=False)
    os.makedirs(multi_root + "/cfg", 0o700)
    multi_env = dict(batch_env)
    multi_env["MINI_AGENT_CONFIG"] = multi_root + "/cfg/config.json"
    multi_env["MINI_AGENT_SESSION_DIR"] = multi_root + "/sess"
    multi_env["MINI_AGENT_LOG"] = multi_root + "/agent.log"
    threading.Thread(target=serve_beta, daemon=True).start()
    time.sleep(0.3)

    mpid, mfd = pty.fork()
    if mpid == 0:
        os.execve(binary, [binary, "run", "--new"], multi_env)
    set_pty_size(mfd, 24, 80)
    mbanner = read_all(mfd, 1.0)
    expect("config 0/3 · next: /provider <url>" in strip_csi(mbanner),
           "multi: empty config points at /provider only")

    def multi_send(keys, marker, tries=16):
        """Send keys, poll until marker renders (fetches are async)."""
        os.write(mfd, keys)
        out = ""
        for _ in range(tries):
            out += read_all(mfd, 0.5)
            if marker in out:
                break
        return out

    added_a = multi_send(f"/provider add alpha http://127.0.0.1:{PORT}/v1\r".encode(),
                         "provider 'alpha' added")
    expect("[ok] provider 'alpha' added" in added_a,
           "multi: /provider add alpha")
    expect("next: /login alpha (key stays hidden)" in added_a,
           "multi: add with url hints /login <name>")
    expect("config 1/3" in strip_csi(added_a),
           "multi: the first profile auto-activates, its url counts toward 1/3")
    added_b = multi_send(f"/provider add beta http://127.0.0.1:{BETA_PORT}/v1\r".encode(),
                         "provider 'beta' added")
    expect("[ok] provider 'beta' added" in added_b, "multi: /provider add beta")

    listed_a = multi_send(b"/login alpha pty-smoke-key\r", "models (2)")
    expect("1. mock-large" in listed_a and "2. mock-small" in listed_a
           and "alpha/mock-large" not in listed_a,
           "multi: single ready profile keeps bare menu ids")
    expect("pty-smoke-key" not in listed_a, "multi: inline key never echoed")
    os.write(mfd, b"\x1b")
    dismiss_a = read_all(mfd, 0.8)
    expect("dismissed" in dismiss_a, "multi: Esc dismisses the alpha menu")

    bad_b = multi_send(b"/login beta wrong-beta-key\r",
                       "model list failed for beta")
    expect("[warn] model list failed for beta:" in bad_b,
           "multi: beta's 401 surfaces as a per-profile warning")
    expect("models (2)" in bad_b and "1. alpha/mock-large" in bad_b
           and "2. alpha/mock-small" in bad_b,
           "multi: failed part still counts -> aggregate qualifies alpha ids")
    expect("wrong-beta-key" not in bad_b, "multi: rejected beta key never echoed")
    os.write(mfd, b"\x1b")
    read_all(mfd, 0.8)

    secret_prompt = multi_send(b"/login beta\r", "[input] API key (beta)")
    expect("[input] API key (beta) (hidden); Enter confirms, empty Enter cancels"
           in secret_prompt,
           "multi: /login <name> opens the masked field pinned to beta")
    os.write(mfd, b"pty-beta-key")
    masked = read_all(mfd, 0.8)
    expect("pty-beta-key" not in masked and "************" in masked,
           "multi: secret field masks the key with stars")
    fixed_b = multi_send(b"\r", "models (4)")
    expect("[ok] api_key updated" in fixed_b, "multi: secret Enter saves the beta key")
    expect("1. alpha/mock-large" in fixed_b and "2. alpha/mock-small" in fixed_b
           and "3. beta/mock-large" in fixed_b and "4. beta/mock-small" in fixed_b,
           "multi: aggregated menu lists both providers, qualified + sorted")
    os.write(mfd, b"\x1b")
    read_all(mfd, 0.8)

    switched = multi_send(b"/model beta/mock-large\r", "model=beta/mock-large")
    expect("model=beta/mock-large" in switched,
           "multi: qualified /model switches profile + model atomically")
    expect("MINI_AGENT_" not in switched, "multi: no env-override warning without env vars")
    ambiguous = multi_send(b"/model mock-small\r", "several providers")
    expect("[error] model 'mock-small' exists on several providers: alpha, beta"
           in ambiguous,
           "multi: ambiguous bare id fails closed, names both owners")
    expect("pick one with /model <provider>/mock-small" in ambiguous,
           "multi: ambiguity error teaches the qualified syntax")

    plist = multi_send(b"/provider list\r", "usage /provider")
    expect("providers (2):" in plist, "multi: /provider list counts both profiles")
    expect("key=***set***" in plist and "pty-smoke-key" not in plist
           and "pty-beta-key" not in plist,
           "multi: /provider list masks keys")
    expect("beta · http://127.0.0.1:8941/v1 · key=***set***  <- active" in plist,
           "multi: /provider list marks the active profile")
    expect("base_url=" not in plist and "(unnamed)" not in plist,
           "multi: /provider list hides the redundant effective-value row when active")
    cfg_view = multi_send(b"/config\r", "providers=2 active=beta")
    expect("providers=2 active=beta" in cfg_view, "multi: /config shows profile count + active")
    # The trailing "config file: ... (env overrides file)" help line always
    # mentions env; only the live status suffix proves an actual override.
    expect("env override in effect" not in cfg_view,
           "multi: /config reports no env override")

    mid_cfg = json.load(open(multi_env["MINI_AGENT_CONFIG"]))
    expect(set(mid_cfg.get("providers", {})) == {"alpha", "beta"}
           and mid_cfg["providers"]["alpha"].get("api_key") == "pty-smoke-key"
           and mid_cfg["providers"]["beta"].get("api_key") == "pty-beta-key"
           and mid_cfg.get("active_provider") == "beta"
           and mid_cfg["providers"]["beta"].get("model") == "mock-large"
           and "model" not in mid_cfg,
           "multi: config file stores both profiles, keys, active, per-profile model")

    tabbed = multi_send(b"/login \t", "options for /login")
    expect("options for /login (cycle with Tab):" in strip_csi(tabbed),
           "multi: Tab on /login lists provider-name choices")
    expect("/login alpha" in strip_csi(tabbed),
           "multi: Tab inserts the first provider name")
    ctrlc = multi_send(b"\x03", "press Ctrl-C again")
    expect("[hint] press Ctrl-C again to quit, or type a new prompt" in ctrlc,
           "multi: single Ctrl-C clears the composer, does not quit")

    removed = multi_send(b"/provider rm beta\r", "provider 'beta' removed")
    # v7 (design 013 Q2): rm of the active profile falls back to the first
    # remaining profile; beta's credentials are gone with it.
    expect("[info] active profile removed; switched to 'alpha'" in removed,
           "multi: removing the active profile switches to the remaining one")
    expect("[ok] provider 'beta' removed" in removed, "multi: /provider rm beta")
    after_rm = multi_send(b"/config\r", "providers=1 active=alpha")
    expect("providers=1 active=alpha" in after_rm,
           "multi: after rm one profile remains and is active")
    plist_after = multi_send(b"/provider list\r", "usage /provider")
    expect("alpha · http://127.0.0.1:8939/v1 · key=***set***  <- active" in plist_after
           and "(unnamed)" not in plist_after and "base_url=" not in plist_after,
           "multi: /provider list shows only the remaining profile, no unnamed row")

    # alpha never had a model selected; pick one explicitly, then run a
    # turn through it. Sessions persist at turn commit, so this also gives
    # the meta.provider assertion a file.
    picked_a = multi_send(b"/model alpha/mock-large\r", "model=alpha/mock-large")
    expect("model=alpha/mock-large" in picked_a,
           "multi: qualified /model reconfigures the remaining profile")
    turn = multi_send(b"hello\r", "You: hello", tries=24)
    expect("tok0" in turn and "You: hello" in turn,
           "multi: a turn works on the remaining profile after rm")
    read_all(mfd, 0.8) # let the post-turn save land before /exit

    os.write(mfd, b"/exit\r")
    exit_out = read_all(mfd, 2.0)
    _, mstatus = os.waitpid(mpid, 0)
    expect(os.WEXITSTATUS(mstatus) == 0, "multi: /exit returns 0")
    expect("saved session" in exit_out and "run --session" in exit_out,
           "multi: exit saves the session with resume hint")
    os.close(mfd)

    mfiles = glob.glob(multi_root + "/sess/*.json")
    expect(len(mfiles) == 1, "multi: one session persisted")
    if mfiles:
        mdata = json.load(open(mfiles[0]))
        expect(mdata["meta"].get("provider") == "alpha"
               and mdata["meta"].get("model") == "mock-large",
               "multi: session meta records the last selected provider/model")
    final_cfg = json.load(open(multi_env["MINI_AGENT_CONFIG"]))
    expect(set(final_cfg.get("providers", {})) == {"alpha"}
           and final_cfg.get("active_provider") == "alpha"
           and final_cfg["providers"]["alpha"].get("api_key") == "pty-smoke-key"
           and final_cfg["providers"]["alpha"].get("model") == "mock-large"
           and "api_key" not in final_cfg,
           "multi: final config keeps only alpha, beta's credentials dropped")

    mlog = open(multi_env["MINI_AGENT_LOG"]).read()
    expect(mlog.count("provider_add ") == 2, "multi: log records both provider_add events")
    expect("login status=verified models=2 provider=alpha" in mlog,
           "multi: log records alpha's verified key")
    expect("login status=rejected models=0 provider=beta http_status=401" in mlog,
           "multi: log records beta's 401 rejection")
    expect("login status=verified models=2 provider=beta" in mlog,
           "multi: log records beta's later accepted key")
    expect(re.search(r"models_list provider=alpha .*status=ok count=2", mlog)
           and re.search(r"models_list provider=beta .*status=ok count=2", mlog),
           "multi: log records per-provider listing outcomes")
    expect("provider_activate provider=beta source=slash" in mlog,
           "multi: log records the qualified /model profile switch")
    expect("model_select model=mock-large source=slash provider=beta" in mlog,
           "multi: log records the selected model with its provider")
    expect("provider_rm provider=beta" in mlog, "multi: log records provider_rm")
    expect("pty-smoke-key" not in mlog and "pty-beta-key" not in mlog
           and "wrong-beta-key" not in mlog,
           "multi: no key value ever reaches the log")

    ghost = subprocess.run([binary, "ask", "--provider", "ghost", "hi"],
                           env=multi_env, text=True, capture_output=True, timeout=15)
    expect(ghost.returncode == 2, "multi: unknown --provider exits with config error 2")
    expect("error: unknown provider 'ghost' (available: alpha)" in ghost.stderr,
           "multi: CLI names the available providers after rm")
    subprocess.run(["rm", "-rf", multi_root], check=False)
    subprocess.run(["rm", "-rf", batch_root], check=False)
    config_dir.cleanup()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
