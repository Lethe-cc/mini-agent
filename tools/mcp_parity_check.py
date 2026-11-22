#!/usr/bin/env python3
"""Backend parity runner (design 021 M1 phase C): run tools/mcp_parity.cpp
scenarios under MINI_AGENT_MCP_BACKEND=legacy and =cxxmcp and diff the
transcripts byte-for-byte. Known-deviation scenarios (marked "deviation"
in the transcript) are compared line-by-line and reported separately.

Dev tool only; NOT a business quality gate. Usage:
  cmake --build --preset linux-gcc-debug
  python3 tools/mcp_parity_check.py [--build-dir build/linux-gcc-debug]

Builds the probe with the same link line as mcp_test (see mcp_parity.cpp
header), runs every scenario twice, prints a per-scenario PASS/DEVIATION/
FAIL table, exits 1 on any unexpected difference.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCENARIOS = [
    "stdio-basic",
    "stdio-die",
    "stdio-spawnfail",
    "stdio-timeout",
    "http-basic",
    "http-401",
    "http-500",
    "http-404mid",
    "http-nonstd",
    "http-noresult",
]
# Scenarios with accepted deviations (design 021 §9); differences inside
# them are reported, not fatal, but any difference OUTSIDE these fails.
KNOWN_DEVIATION = {"http-404mid", "http-nonstd", "http-noresult"}


def build_probe(build_dir: Path, out: Path) -> None:
    cmd = [
        "g++", "-std=c++17", "-g", "-Iinclude",
        "tools/mcp_parity.cpp", "-o", str(out),
        str(build_dir / "libagent_core.a"),
        str(build_dir / "third_party/cxxmcp/libmcp_client.a"),
        str(build_dir / "third_party/cxxmcp/libmcp_protocol.a"),
        "-lcurl", "-lssl", "-lcrypto", "-pthread",
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)


def run(probe: Path, scenario: str, backend: str) -> str:
    env = dict(os.environ, MINI_AGENT_MCP_BACKEND=backend)
    proc = subprocess.run([str(probe), scenario], cwd=ROOT, env=env,
                          capture_output=True, text=True, timeout=120)
    if proc.returncode != 0:
        raise RuntimeError(f"{scenario}/{backend} exited {proc.returncode}: "
                           f"{proc.stderr[-500:]}")
    return proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build/linux-gcc-debug")
    args = parser.parse_args()
    build_dir = ROOT / args.build_dir
    if not (build_dir / "libagent_core.a").exists():
        print(f"error: {build_dir}/libagent_core.a missing; build first")
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="mcp_parity_") as tmp:
        probe = Path(tmp) / "mcp_parity"
        build_probe(build_dir, probe)
        for scenario in SCENARIOS:
            legacy = run(probe, scenario, "legacy")
            cxxmcp = run(probe, scenario, "cxxmcp")
            (Path(tmp) / f"{scenario}.legacy.txt").write_text(legacy)
            (Path(tmp) / f"{scenario}.cxxmcp.txt").write_text(cxxmcp)
            if legacy == cxxmcp:
                print(f"PASS      {scenario}")
                continue
            if scenario in KNOWN_DEVIATION:
                print(f"DEVIATION {scenario} (accepted, design 021 §9)")
                for tag, text in (("legacy", legacy), ("cxxmcp", cxxmcp)):
                    for ln in text.splitlines():
                        print(f"  {tag}: {ln}")
                continue
            failures += 1
            print(f"FAIL      {scenario}")
            import difflib
            for ln in difflib.unified_diff(legacy.splitlines(),
                                           cxxmcp.splitlines(),
                                           "legacy", "cxxmcp", lineterm=""):
                print(f"  {ln}")
    if failures:
        print(f"{failures} scenario(s) diverged")
        return 1
    print("parity check done: all non-deviation scenarios byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())