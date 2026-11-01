"""Read-only environment diagnosis for the template tools."""

from __future__ import annotations

import shutil
import sys

if __package__:
    from .common import REQUIRED_FILES, ROOT, load_config
else:
    from common import REQUIRED_FILES, ROOT, load_config


def main() -> int:
    errors = []
    print(f"Repository: {ROOT}")
    print(f"Python: {sys.version.split()[0]} ({sys.executable})")
    if sys.version_info < (3, 11):
        errors.append("Python 3.11+ is required")
    git = shutil.which("git")
    print(f"Git (optional): {git or 'not found; required only for Git operations'}")
    for name in REQUIRED_FILES:
        if not (ROOT / name).is_file():
            errors.append(f"missing file: {name}")
    try:
        config = load_config(ROOT)
        print(f"Template version: {config['template_version']}")
        print(f"Initialized: {config['initialized']}")
        language = config.get("language", "generic")
        print(f"Language profile: {language}")
        tools = {
            "cpp": ("cmake", "ctest", "ninja", "clang-format", "clang-tidy"),
            "typescript": ("node", "npm"),
            "go": ("go", "gofmt"),
            "rust": ("cargo", "rustc", "rustup"),
        }.get(language, ())
        for tool in tools:
            print(f"Tool availability ({tool}): {shutil.which(tool) or 'not on PATH'}")
        if tools:
            print("NOTE: availability is not version/compiler compatibility validation; see docs/languages.")
        print(f"Project quality commands: {len(config['quality_commands'])}")
        if not config["initialized"]:
            print("NEXT: run scripts/init_project.py before business development")
        if not config["quality_commands"]:
            print("WARNING: business quality gates are not configured")
    except (OSError, ValueError) as exc:
        errors.append(f"configuration: {exc}")
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    print("No files changed, no external sources accessed, no project commands executed.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
