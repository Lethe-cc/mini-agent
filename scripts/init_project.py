"""Initialize metadata and an optional create-only scaffold; never install or fetch."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

if __package__:
    from .common import PROJECT_TOKENS, ROOT, load_config, read_text, validate_config, validate_metadata, write_text
    from .profiles import DEFAULT_STACKS, LANGUAGES, profile_changes
else:
    from common import PROJECT_TOKENS, ROOT, load_config, read_text, validate_config, validate_metadata, write_text
    from profiles import DEFAULT_STACKS, LANGUAGES, profile_changes


def markdown_literal(value: str) -> str:
    return re.sub(r"([\\`*_{}\[\]()<>#!|])", r"\\\1", value)


def initialize(
    root: Path, name: str, description: str, stack: str,
    dry_run: bool = False, language: str = "generic",
) -> list[str]:
    root = root.resolve()
    validate_metadata(name, description, stack)
    config_path = root / "project.json"
    context_path = root / "docs/project-context.md"
    if config_path.is_symlink() or context_path.is_symlink() or (root / "docs").is_symlink():
        raise ValueError("refusing to initialize through symlinks")
    config = load_config(root)
    if config["initialized"]:
        raise ValueError("project is already initialized; edit project metadata manually, no files changed")
    context = read_text(context_path)
    values = {"name": name, "description": description, "stack": stack}
    for token, key in PROJECT_TOKENS.items():
        if context.count(token) != 1:
            raise ValueError(f"expected exactly one {token} in docs/project-context.md; no files changed")
    for token, key in PROJECT_TOKENS.items():
        context = context.replace(token, markdown_literal(values[key]))
    scaffold, commands = profile_changes(root, language, name)
    if language != "generic" and config["quality_commands"]:
        raise ValueError("profile refuses to replace existing quality_commands")
    updated = validate_config({
        **config, **values, "initialized": True, "language": language,
        "quality_commands": commands if language != "generic" else config["quality_commands"],
    })
    changes = {
        config_path: json.dumps(updated, ensure_ascii=False, indent=2) + "\n",
        context_path: context,
        **scaffold,
    }
    if not dry_run:
        originals = {path: path.read_bytes() if path.exists() else None for path in changes}
        written = []
        created_dirs = []
        try:
            for path, text in changes.items():
                for parent in reversed(path.parents):
                    if parent.is_relative_to(root) and not parent.exists():
                        parent.mkdir()
                        created_dirs.append(parent)
                written.append(path)
                write_text(path, text)
        except OSError as exc:
            rollback_errors = []
            for path in reversed(written):
                try:
                    if originals[path] is None:
                        path.unlink(missing_ok=True)
                    else:
                        path.write_bytes(originals[path])
                except OSError as rollback_error:
                    rollback_errors.append(str(rollback_error))
            for directory in reversed(created_dirs):
                try:
                    directory.rmdir()
                except OSError as rollback_error:
                    rollback_errors.append(str(rollback_error))
            if rollback_errors:
                raise OSError(f"initialization failed: {exc}; rollback incomplete: {rollback_errors}") from exc
            raise
    return [str(path.relative_to(root)) for path in changes]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", help="lowercase kebab-case project slug")
    parser.add_argument("--description", help="one-line project goal")
    parser.add_argument("--stack", help="chosen stack; defaults to the selected language's starter stack")
    parser.add_argument("--language", choices=LANGUAGES, default="generic", help="optional scaffold (default: generic)")
    parser.add_argument("--dry-run", action="store_true", help="validate and preview without writes")
    args = parser.parse_args()
    if sys.version_info < (3, 11):
        print("ERROR: Python 3.11+ is required", file=sys.stderr)
        return 1
    try:
        values = {}
        for key, label in (("name", "Project slug"), ("description", "One-line goal"), ("stack", "Stack")):
            value = getattr(args, key)
            if value is None and key == "stack":
                value = DEFAULT_STACKS[args.language]
            if value is None:
                if not sys.stdin.isatty():
                    raise ValueError(f"--{key} is required in non-interactive mode")
                value = input(f"{label}: ").strip()
            values[key] = value
        paths = initialize(ROOT, **values, dry_run=args.dry_run, language=args.language)
    except (OSError, ValueError, EOFError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print("Preview only; no files changed." if args.dry_run else "Project initialized.")
    for path in paths:
        print(f"  {path}")
    print(f"Name: {values['name']} | Language: {args.language} | Stack: {values['stack']}")
    print(f"Goal: {values['description']}")
    print("Next: read docs/languages/README.md, complete context, and verify the selected toolchain.")
    print("Starter tests verify scaffolding only, not your business requirements. No dependencies were installed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
