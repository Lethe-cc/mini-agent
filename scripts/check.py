"""Run baseline checks, tooling tests, then explicitly configured project gates."""

from __future__ import annotations

import argparse
import subprocess
import sys

if __package__:
    from .common import ROOT, baseline_errors, load_config
else:
    from common import ROOT, baseline_errors, load_config


def run_command(name: str, argv: list[str], timeout: int) -> bool:
    argv = list(argv)
    if argv[0] == "python":
        argv[0] = sys.executable
    print(f"RUN {name}: {argv!r}", flush=True)
    try:
        result = subprocess.run(argv, cwd=ROOT, shell=False, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL {name}: {exc}", file=sys.stderr)
        return False
    if result.returncode:
        print(f"FAIL {name}: exit {result.returncode}", file=sys.stderr)
        return False
    print(f"PASS {name}", flush=True)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-initialized", action="store_true")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--strict-project", action="store_true", help="also require at least one project gate")
    mode.add_argument("--tooling-only", action="store_true", help="explicitly skip project commands; not project validation")
    args = parser.parse_args()
    if sys.version_info < (3, 11):
        print("ERROR: Python 3.11+ is required", file=sys.stderr)
        return 1
    try:
        errors = baseline_errors(ROOT, args.require_initialized, args.strict_project)
        if errors:
            for error in errors:
                print(f"FAIL: {error}", file=sys.stderr)
            return 1
        config = load_config(ROOT)
    except (OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print("PASS baseline configuration and document checks", flush=True)
    if not config["initialized"]:
        print("WARNING: template mode; new-project initialization is still required", flush=True)
    passed = run_command(
        "template tooling tests",
        [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"],
        120,
    )
    if args.tooling_only:
        print("WARNING: --tooling-only explicitly skips ALL project commands", flush=True)
    for command in ([] if args.tooling_only else config["quality_commands"]):
        # Run every configured gate so a failure does not hide later results.
        result = run_command(command["name"], command["argv"], command.get("timeout_seconds", 300))
        passed = result and passed
    if not config["quality_commands"]:
        print("WARNING: no business quality commands; this is NOT business validation", flush=True)
    success = "PASS tooling-only checks (project commands skipped)" if args.tooling_only else "PASS all configured checks"
    print(success if passed else "FAIL one or more checks", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
