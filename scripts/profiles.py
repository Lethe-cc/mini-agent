"""Load explicit, create-only language scaffolds from local profile manifests."""

from __future__ import annotations

import json
from pathlib import Path, PurePosixPath

LANGUAGES = ("generic", "cpp", "python", "typescript", "go", "rust")
DEFAULT_STACKS = {
    "generic": "Undecided",
    "cpp": "C++17 / CMake / CTest",
    "python": "Python 3.11+ / unittest",
    "typescript": "TypeScript / Node.js / node:test",
    "go": "Go 1.23+ / modules / testing",
    "rust": "Rust 1.85+ / edition 2024 / Cargo",
}


def confined_path(base: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
        raise ValueError("profile paths must be relative POSIX paths")
    pure = PurePosixPath(relative)
    if pure.is_absolute() or ".." in pure.parts or pure.as_posix() != relative or relative == ".":
        raise ValueError(f"unsafe profile path: {relative}")
    path = base.joinpath(*pure.parts)
    current = base
    for part in pure.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"profile paths must not traverse symlinks: {relative}")
    if not path.resolve().is_relative_to(base.resolve()):
        raise ValueError(f"profile path escapes root: {relative}")
    return path


def profile_changes(root: Path, language: str, name: str) -> tuple[dict[Path, str], list[dict]]:
    if language not in LANGUAGES:
        raise ValueError(f"unsupported language: {language}")
    if language == "generic":
        return {}, []
    base = confined_path(root, f"profiles/{language}")
    manifest = json.loads(confined_path(base, "profile.json").read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or not isinstance(manifest.get("files"), dict):
        raise ValueError("profile manifest must contain a files object")
    changes = {}
    for destination, source in manifest["files"].items():
        path = confined_path(root, destination)
        if path.exists():
            raise ValueError(f"profile refuses to overwrite existing path: {destination}")
        for parent in path.parents:
            if parent == root:
                break
            if parent.exists() and not parent.is_dir():
                raise ValueError(f"profile parent is not a directory: {parent}")
        text = confined_path(base, source).read_text(encoding="utf-8")
        changes[path] = text.replace("@@PROJECT_NAME@@", name)
    return changes, manifest.get("quality_commands")
