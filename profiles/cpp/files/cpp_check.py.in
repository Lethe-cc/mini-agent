"""Fail-fast configure/build/test pipeline; no downloads or dependency installation."""

import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PRESETS = (
    "dev", "release", "linux-gcc-debug", "linux-gcc-release",
    "linux-clang-debug", "linux-clang-release",
)


def default_preset():
    if sys.platform.startswith("linux"):
        return "linux-gcc-debug"
    return "dev"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=PRESETS, default=os.environ.get(
        "CPP_PRESET", default_preset()
    ))
    parser.add_argument("--tidy", action="store_true")
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--warnings-as-errors", action="store_true")
    args = parser.parse_args()
    if args.preset not in PRESETS:
        parser.error("CPP_PRESET is not a supported preset")
    commands = [
        ["cmake", "--preset", args.preset,
         f"-DSTARTER_CLANG_TIDY={'ON' if args.tidy else 'OFF'}",
         f"-DSTARTER_SANITIZERS={'ON' if args.sanitizers else 'OFF'}",
         f"-DSTARTER_WARNINGS_AS_ERRORS={'ON' if args.warnings_as_errors else 'OFF'}"],
        ["cmake", "--build", "--preset", args.preset, "--parallel", "2"],
        ["ctest", "--preset", args.preset, "--no-tests=error"],
    ]
    for command in commands:
        print(f"RUN {command!r}", flush=True)
        try:
            result = subprocess.run(command, cwd=ROOT, shell=False, check=False, timeout=240)
        except (OSError, subprocess.TimeoutExpired) as exc:
            print(f"FAIL: {exc}", file=sys.stderr)
            return 1
        if result.returncode:
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
