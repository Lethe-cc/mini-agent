"""Shared validation helpers. No network access or dependency installation."""

from __future__ import annotations

import json
import os
import re
from pathlib import Path
from urllib.parse import unquote, urlsplit

if __package__:
    from .profiles import LANGUAGES
else:
    from profiles import LANGUAGES

ROOT = Path(__file__).resolve().parents[1]
NAME_PATTERN = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
TOKEN_PATTERN = re.compile(r"\{\{[A-Z][A-Z0-9_]*\}\}")
PROJECT_TOKENS = {
    "{{PROJECT_NAME}}": "name",
    "{{PROJECT_DESCRIPTION}}": "description",
    "{{PROJECT_STACK}}": "stack",
}
REQUIRED_FILES = (
    "README.md",
    "AGENTS.md",
    "CLAUDE.md",
    ".github/copilot-instructions.md",
    "docs/project-context.md",
    "docs/architecture.md",
    "docs/development.md",
    "docs/ai-onboarding.md",
    "docs/references/README.md",
    "docs/design/README.md",
    "docs/decisions/README.md",
    "docs/template-maintenance.md",
    "templates/task.md",
    "templates/design.md",
    "templates/adr.md",
    "templates/reference.md",
    "workflows/implement-feature.md",
    "workflows/design-feature.md",
    "workflows/fix-bug.md",
    "workflows/review-change.md",
    "workflows/finalize-design.md",
    "scripts/init_project.py",
    "scripts/doctor.py",
    "scripts/check.py",
    "scripts/common.py",
    "scripts/profiles.py",
    "docs/languages/README.md",
    "docs/languages/cpp.md",
    "docs/languages/python.md",
    "docs/languages/typescript.md",
    "docs/languages/go.md",
    "docs/languages/rust.md",
    "tests/test_template.py",
)
SKIP_DIRS = {
    ".git", ".references", ".venv", "venv", "node_modules", "__pycache__",
    ".pi", "dist", "build", "coverage", ".pytest_cache", "target", "vendor",
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def validate_metadata(name: str, description: str, stack: str) -> None:
    if not isinstance(name, str) or len(name) > 64 or not NAME_PATTERN.fullmatch(name):
        raise ValueError("name must be a lowercase kebab-case slug, at most 64 characters")
    for key, value in (("description", description), ("stack", stack)):
        if not isinstance(value, str) or not value.strip() or len(value) > 500:
            raise ValueError(f"{key} must contain 1-500 nonblank characters")
        if any(ord(char) < 32 or ord(char) == 127 for char in value):
            raise ValueError(f"{key} must be a single line without control characters")
        if TOKEN_PATTERN.search(value):
            raise ValueError(f"{key} must not contain template placeholders")


def validate_config(config: object) -> dict:
    if not isinstance(config, dict):
        raise ValueError("project.json must be an object")
    if type(config.get("schema_version")) is not int or config["schema_version"] != 1:
        raise ValueError("unsupported schema_version (expected 1)")
    if not isinstance(config.get("template_version"), str) or not config["template_version"].strip():
        raise ValueError("template_version must be a nonempty string")
    if type(config.get("initialized")) is not bool:
        raise ValueError("initialized must be a boolean")
    if config.get("language", "generic") not in LANGUAGES:
        raise ValueError(f"language must be one of {LANGUAGES}")
    if config["initialized"]:
        validate_metadata(config.get("name"), config.get("description"), config.get("stack"))
    elif any(config.get(key) is not None for key in ("name", "description", "stack")):
        raise ValueError("uninitialized metadata must be null; use init_project.py")
    commands = config.get("quality_commands")
    if not isinstance(commands, list):
        raise ValueError("quality_commands must be an array")
    names = set()
    for command in commands:
        if not isinstance(command, dict):
            raise ValueError("each quality command must be an object")
        name = command.get("name")
        if not isinstance(name, str) or not name.strip() or name in names:
            raise ValueError("quality command names must be nonempty and unique")
        names.add(name)
        argv = command.get("argv")
        if not isinstance(argv, list) or not argv or any(
            not isinstance(arg, str) or not arg or "\x00" in arg for arg in argv
        ):
            raise ValueError(f"{name}: argv must be a nonempty array of nonempty strings")
        timeout = command.get("timeout_seconds", 300)
        if type(timeout) is not int or not 1 <= timeout <= 3600:
            raise ValueError(f"{name}: timeout_seconds must be an integer from 1 to 3600")
    return config


def load_config(root: Path) -> dict:
    return validate_config(json.loads(read_text(root / "project.json")))


def markdown_files(root: Path):
    """Walk first-party Markdown without following directory symlinks."""
    for directory, dirs, files in os.walk(root, followlinks=False):
        base = Path(directory)
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not (base / d).is_symlink())
        for filename in sorted(files):
            path = base / filename
            if path.suffix.lower() == ".md" and not path.is_symlink():
                yield path


def prose_lines(text: str):
    """Ignore fenced blocks; not a complete CommonMark parser."""
    fence_char = None
    fence_size = 0
    for number, line in enumerate(text.splitlines(), 1):
        marker = re.match(r"^\s{0,3}(`{3,}|~{3,})(.*)$", line)
        if marker:
            run, tail = marker.groups()
            if fence_char is None:
                fence_char, fence_size = run[0], len(run)
                continue
            if run[0] == fence_char and len(run) >= fence_size and not tail.strip():
                fence_char = None
                continue
        if fence_char is None:
            yield number, line


def markdown_errors(root: Path, initialized: bool) -> list[str]:
    errors = []
    root = root.resolve()
    link_pattern = re.compile(r"!?\[[^\]\n]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+[\"'][^\n]*[\"'])?\s*\)")
    for path in markdown_files(root):
        relative = path.relative_to(root)
        text = read_text(path)
        for number, line in enumerate(text.splitlines(), 1):
            if line.rstrip(" \t") != line:
                errors.append(f"{relative}:{number}: trailing whitespace")
            if any((ord(char) < 32 and char != "\t") or ord(char) == 127 for char in line):
                errors.append(f"{relative}:{number}: control character")
        if initialized and relative.parts[0] == "docs" and TOKEN_PATTERN.search(text):
            errors.append(f"{relative}: unresolved template placeholder")
        for number, line in prose_lines(text):
            # Inline code may contain illustrative Markdown, not actual links.
            line = re.sub(r"(`+).*?\1", "", line)
            for match in link_pattern.finditer(line):
                destination = match.group(1).strip("<>")
                try:
                    parsed = urlsplit(destination)
                except ValueError:
                    errors.append(f"{relative}:{number}: malformed link {destination}")
                    continue
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                if parsed.path.startswith("/"):
                    # Site-root URLs have renderer-specific semantics.
                    continue
                target = (path.parent / unquote(parsed.path)).resolve()
                if not target.is_relative_to(root):
                    errors.append(f"{relative}:{number}: link escapes repository: {destination}")
                elif not target.exists():
                    errors.append(f"{relative}:{number}: missing link target: {destination}")
    return errors


def baseline_errors(root: Path, require_initialized: bool = False, strict: bool = False) -> list[str]:
    errors = [f"missing required file: {name}" for name in REQUIRED_FILES if not (root / name).is_file()]
    try:
        config = load_config(root)
    except (OSError, ValueError) as exc:
        return errors + [f"configuration: {exc}"]
    if (require_initialized or strict) and not config["initialized"]:
        errors.append("project is not initialized; run scripts/init_project.py")
    if strict and not config["quality_commands"]:
        errors.append("no project quality commands configured; template checks are not business validation")
    errors.extend(markdown_errors(root, config["initialized"]))
    return errors
