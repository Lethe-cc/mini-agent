#!/usr/bin/env python3
"""Reproduce the v3 approval diff preview end-to-end without a real LLM.

Dev tool per AGENTS.md (manual verification aid; NOT a quality gate, not
wired into project.json). It builds a throwaway git workspace whose file
matches the in-repo mock CODEFIX script (tests/mock_openai_server.py:
read_file -> edit_file -> git_status -> git_add -> git_commit), then drives
`mini-agent ask --tools ask` inside a real pty so the approval block
renders exactly as a human sees it.

Modes:
  auto (default)  two scripted runs:
      RUN 1 answers 'n' to every prompt  -> expect hello.txt UNCHANGED
      RUN 2 answers 'y' to every prompt  -> expect the edit applied AND a
                                            local commit created
  interactive     one run; prompts reach YOUR keyboard so you approve by
                  hand after reading the diff.

Usage:
  cmake --build --preset linux-gcc-debug
  python3 tools/diff_demo.py                 # automated deny/allow proof
  python3 tools/diff_demo.py interactive     # you decide y/n yourself
"""
import os
import pty
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build/linux-gcc-debug/mini-agent"
MOCK = ROOT / "tests/mock_openai_server.py"
PORT = 8946


def free_port(candidate=PORT):
    with socket.socket() as s:
        try:
            s.bind(("127.0.0.1", candidate))
            return candidate
        except OSError:
            return free_port(candidate + 1)


def make_workspace(tag):
    ws = Path(tempfile.gettempdir()) / f"mini_agent_diff_demo_{tag}"
    if ws.exists():
        subprocess.run(["rm", "-rf", str(ws)], check=True)
    ws.mkdir()
    (ws / "hello.txt").write_text("CHANGEME\nsecond line\n")

    def run(*a):
        subprocess.run(a, cwd=ws, check=True, capture_output=True)

    run("git", "init", "-q", ".")
    run("git", "config", "user.email", "demo@demo")
    run("git", "config", "user.name", "demo")
    run("git", "add", "hello.txt")
    run("git", "commit", "-qm", "init")
    return ws


def drive(answer_key, ws, env, overall_timeout=60):
    """Answer EXACTLY ONE key per prompt appearance (count-based, so a
    fast loop can never double-answer and desync the y/N stream)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(str(BIN), [str(BIN), "ask", "--tools", "ask", "--cwd", str(ws),
                             "CODEFIX demo"], env)
    buf = b""
    answered = 0
    end = time.time() + overall_timeout
    try:
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.3)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk
                seen = buf.count(b"allow? [y/N]")
                while answered < seen:
                    time.sleep(0.3)  # let the block render before replying
                    os.write(fd, answer_key)
                    answered += 1
                if b"edited and committed" in buf:
                    break
            elif answered and buf.count(b"allow? [y/N]") == answered:
                # quiet with every prompt answered: deny-run ends when the
                # model gives up asking (its final text) or goes idle long
                if b"finish" in buf or time.time() > end - 6:
                    break
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass
    return buf.decode(errors="replace")


CSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")


def approval_blocks(raw):
    text = CSI.sub("", raw).replace("\r\n", "\n").replace("\r", "\n")
    blocks, pos = [], 0
    while True:
        start = text.find("[approve]", pos)
        if start < 0:
            break
        end = text.find("allow? [y/N]", start)
        if end < 0:
            break
        blocks.append(text[start:end + len("allow? [y/N]")])
        pos = end + 1
    return blocks


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "auto"
    if not BIN.exists():
        print(f"FAIL: build the binary first: {BIN}")
        return 1
    port = free_port()
    mock = subprocess.Popen([sys.executable, str(MOCK), str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.7)
    cfgdir = Path(tempfile.mkdtemp(prefix="mini_agent_diff_demo_cfg_"))
    env = dict(os.environ,
               MINI_AGENT_BASE_URL=f"http://127.0.0.1:{port}/v1",
               MINI_AGENT_API_KEY="demo", MINI_AGENT_MODEL="mock",
               MINI_AGENT_CONFIG=str(cfgdir / "config.json"),
               MINI_AGENT_SESSION_DIR=str(cfgdir / "sessions"),
               MINI_AGENT_LOG=str(cfgdir / "agent.log"))
    env.pop("NO_COLOR", None)
    env["TERM"] = "xterm"
    failures = []
    ws_n = ws_y = None
    try:
        if mode == "interactive":
            ws = make_workspace("interactive")
            print(f"workspace: {ws}\napprovals go to YOUR keyboard.\n")
            proc = subprocess.run([str(BIN), "ask", "--tools", "ask", "--cwd", str(ws),
                                   "CODEFIX demo"], env=env)
            print(f"\nhello.txt now: {(ws / 'hello.txt').read_text()!r}")
            log = subprocess.run(["git", "log", "--oneline"], cwd=ws,
                                 capture_output=True, text=True).stdout
            print("commits:\n" + log)
            return proc.returncode

        ws_n = make_workspace("deny")
        blocks_n = approval_blocks(drive(b"n\n", ws_n, env))
        ws_y = make_workspace("allow")
        blocks_y = approval_blocks(drive(b"y\n", ws_y, env))
        print(f"(deny run: {len(blocks_n)} approval prompts answered; "
              f"allow run: {len(blocks_y)})")

        print("=" * 60)
        print("RUN 1 - answer 'n' to every approval (deny path)")
        if blocks_n:
            print(blocks_n[0].strip())
        else:
            failures.append("deny run showed no approval block")
        hello_n = (ws_n / "hello.txt").read_text()
        print(f"\nhello.txt after denials: {hello_n!r}")
        print("OK: denied edit never touched the file" if "CHANGEME" in hello_n
              else "FAIL: denied edit LEAKED to disk")
        if "CHANGEME" not in hello_n:
            failures.append("denied edit leaked")

        print("=" * 60)
        print("RUN 2 - answer 'y' to every approval (allow path)")
        edit_block = next((b for b in blocks_y if "edit_file" in b), None)
        print(edit_block.strip() if edit_block else "FAIL: no edit_file diff block!")
        if not edit_block:
            failures.append("allow run missing edit_file diff")
        hello_y = (ws_y / "hello.txt").read_text()
        print(f"\nhello.txt after approvals: {hello_y!r}")
        if "changed by mini-agent" in hello_y:
            print("OK: approved edit landed exactly as the diff previewed")
        else:
            failures.append("approved edit did not land")
        commits = subprocess.run(["git", "log", "--oneline"], cwd=ws_y,
                                 capture_output=True, text=True).stdout
        print("git log: " + commits.strip())
        if "update hello.txt wording" in commits:
            print("OK: approved git_commit created the local commit")
        else:
            failures.append("approved commit missing")

        print("=" * 60)
        logpath = cfgdir / "agent.log"
        if logpath.exists():
            logtext = logpath.read_text()
            if "changed by mini-agent" in logtext or "CHANGEME" in logtext:
                failures.append("file CONTENT leaked into the log")
            else:
                print("OK: log carries metadata only (no file contents)")
        print("RESULT: " + ("; ".join(failures) if failures
                            else "ALL CHECKS PASSED (deny keeps disk clean; allow applies the diff)"))
        return 1 if failures else 0
    finally:
        mock.send_signal(signal.SIGTERM)
        leftovers = [str(p) for p in (ws_n, ws_y) if p]
        leftovers += [str(cfgdir)] + [str(x) for x in
                                      Path(tempfile.gettempdir()).glob("mini_agent_diff_demo_*")]
        subprocess.run(["rm", "-rf", *leftovers], check=False)


if __name__ == "__main__":
    raise SystemExit(main())
