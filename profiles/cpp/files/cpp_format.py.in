"""Check first-party C++ formatting without rewriting files."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    files = []
    for directory in ("include", "src", "tests/cpp"):
        for path in (ROOT / directory).rglob("*"):
            if path.suffix in (".h", ".hpp", ".cpp", ".cc", ".cxx") and path.is_file():
                files.append(str(path.relative_to(ROOT)))
    if not files:
        print("FAIL: no C++ files found", file=sys.stderr)
        return 1
    try:
        return subprocess.run(
            ["clang-format", "--dry-run", "--Werror", *sorted(files)],
            cwd=ROOT, shell=False, check=False, timeout=120,
        ).returncode
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
