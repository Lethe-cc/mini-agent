"""Regression tests for template utilities; not application behavior tests."""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import check
from scripts.common import (
    REQUIRED_FILES,
    ROOT,
    baseline_errors,
    load_config,
    markdown_errors,
    validate_config,
    write_text,
)
from scripts.init_project import initialize


def template_config() -> dict:
    return {
        "schema_version": 1,
        "template_version": "0.1.0",
        "initialized": False,
        "name": None,
        "description": None,
        "stack": None,
        "quality_commands": [],
    }


class TemplateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ai-template-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in REQUIRED_FILES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            write_text(path, "")
        write_text(self.root / "project.json", json.dumps(template_config()))
        write_text(
            self.root / "docs/project-context.md",
            "# Context\n{{PROJECT_NAME}}\n{{PROJECT_DESCRIPTION}}\n{{PROJECT_STACK}}\nKeep this section.\n",
        )

    def initialize(self, **kwargs):
        return initialize(self.root, "demo-project", "中文 project goal", "Undecided", **kwargs)

    def test_initialization_updates_only_declared_files(self):
        sentinel = self.root / "src.txt"
        write_text(sentinel, "{{PROJECT_NAME}} do not replace")
        paths = self.initialize()
        self.assertEqual(set(paths), {"project.json", str(Path("docs/project-context.md"))})
        self.assertTrue(load_config(self.root)["initialized"])
        context = (self.root / "docs/project-context.md").read_text(encoding="utf-8")
        self.assertIn("中文 project goal", context)
        self.assertIn("Keep this section.", context)
        self.assertNotIn("{{PROJECT_NAME}}", context)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "{{PROJECT_NAME}} do not replace")

    def test_dry_run_changes_nothing(self):
        before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        self.initialize(dry_run=True)
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})

    def test_reinitialization_is_refused_without_changes(self):
        self.initialize()
        before = (self.root / "project.json").read_bytes()
        with self.assertRaisesRegex(ValueError, "already initialized"):
            self.initialize()
        self.assertEqual(before, (self.root / "project.json").read_bytes())

    def test_missing_or_duplicate_token_prevents_write(self):
        for text in ("no tokens", "{{PROJECT_NAME}} {{PROJECT_NAME}}"):
            write_text(self.root / "docs/project-context.md", text)
            before = (self.root / "project.json").read_bytes()
            with self.assertRaisesRegex(ValueError, "exactly one"):
                self.initialize()
            self.assertEqual(before, (self.root / "project.json").read_bytes())

    def test_invalid_metadata(self):
        for name in ("../escape", "Bad Name", "demo--name", "x" * 65, ""):
            with self.subTest(name=name), self.assertRaises(ValueError):
                initialize(self.root, name, "goal", "stack")
        for description in ("", " \t", "two\nlines", "{{UNRESOLVED}}", "bad\x00text"):
            with self.subTest(description=description), self.assertRaises(ValueError):
                initialize(self.root, "demo", description, "stack")

    def test_metadata_markdown_is_literal(self):
        initialize(self.root, "demo", "[a](missing.md) <tag>", "a_b")
        self.assertEqual(markdown_errors(self.root, True), [])
        self.assertIn("\\[a\\]", (self.root / "docs/project-context.md").read_text(encoding="utf-8"))

    def test_write_failure_restores_originals(self):
        before = {name: (self.root / name).read_bytes() for name in ("project.json", "docs/project-context.md")}
        calls = 0

        def fail_second(path, text):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("simulated write failure")
            write_text(path, text)

        with patch("scripts.init_project.write_text", side_effect=fail_second):
            with self.assertRaises(OSError):
                self.initialize()
        self.assertEqual(before, {name: (self.root / name).read_bytes() for name in before})

    def test_template_and_strict_modes(self):
        self.assertEqual(baseline_errors(self.root), [])
        self.assertTrue(baseline_errors(self.root, require_initialized=True))
        self.initialize()
        self.assertEqual(baseline_errors(self.root, require_initialized=True), [])
        self.assertTrue(any("no project quality" in e for e in baseline_errors(self.root, strict=True)))
        config = load_config(self.root)
        config["quality_commands"] = [{"name": "example", "argv": ["python", "--version"]}]
        write_text(self.root / "project.json", json.dumps(config))
        self.assertEqual(baseline_errors(self.root, strict=True), [])

    def test_config_rejects_bad_shapes(self):
        invalid = [None, [], {**template_config(), "schema_version": True},
                   {**template_config(), "initialized": "false"},
                   {**template_config(), "name": "premature"},
                   {**template_config(), "quality_commands": "echo hello"}]
        for config in invalid:
            with self.subTest(config=config), self.assertRaises(ValueError):
                validate_config(config)

    def test_config_rejects_bad_commands(self):
        for command in (
            {"name": "x", "argv": "python --version"},
            {"name": "x", "argv": []},
            {"name": "x", "argv": ["python"], "timeout_seconds": True},
            {"name": "x", "argv": ["python"], "timeout_seconds": 0},
            {"name": "x", "argv": ["python"], "timeout_seconds": 3601},
        ):
            with self.subTest(command=command), self.assertRaises(ValueError):
                validate_config({**template_config(), "quality_commands": [command]})
        command = {"name": "same", "argv": ["python"]}
        with self.assertRaises(ValueError):
            validate_config({**template_config(), "quality_commands": [command, command]})

    def test_missing_required_file(self):
        (self.root / "AGENTS.md").unlink()
        self.assertTrue(any("AGENTS.md" in error for error in baseline_errors(self.root)))

    def test_broken_link_is_reported(self):
        write_text(self.root / "README.md", "[missing](docs/missing.md)\n")
        self.assertTrue(any("missing link target" in e for e in markdown_errors(self.root, False)))

    def test_links_allow_local_encoded_paths_and_ignore_fences(self):
        write_text(self.root / "docs/a b.md", "# Heading\n")
        write_text(self.root / "README.md", "[ok](docs/a%20b.md#heading)\n"
                   "[web](https://example.com/not-fetched)\n[anchor](#heading)\n"
                   "```markdown\n[example](missing.md)\n```\n"
                   "`[example](also-missing.md)`\n")
        self.assertEqual(markdown_errors(self.root, False), [])

    def test_link_escape_is_rejected(self):
        write_text(self.root / "README.md", "[escape](../outside.md)\n")
        self.assertTrue(any("escapes repository" in e for e in markdown_errors(self.root, False)))

    def test_placeholders_allowed_only_in_templates_after_init(self):
        self.initialize()
        write_text(self.root / "templates/example.md", "{{ALLOWED}}\n")
        self.assertEqual(markdown_errors(self.root, True), [])
        write_text(self.root / "docs/example.md", "{{NOT_ALLOWED}}\n")
        self.assertTrue(any("placeholder" in e for e in markdown_errors(self.root, True)))

    def test_reference_checkout_is_excluded(self):
        directory = self.root / ".references/vendor"
        directory.mkdir(parents=True)
        write_text(directory / "README.md", "[broken](missing.md)  \n")
        self.assertEqual(markdown_errors(self.root, False), [])

    def test_trailing_whitespace_is_reported(self):
        write_text(self.root / "README.md", "bad  \n")
        self.assertTrue(any("trailing whitespace" in e for e in markdown_errors(self.root, False)))

    def test_repository_itself_has_valid_baseline(self):
        self.assertEqual(baseline_errors(ROOT), [])


class CommandTests(unittest.TestCase):
    def test_nonzero_exit_fails_without_shell(self):
        with patch("scripts.check.subprocess.run", return_value=subprocess.CompletedProcess([], 2)) as run:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertFalse(check.run_command("failure", ["python", "--version"], 10))
            self.assertFalse(run.call_args.kwargs["shell"])
            self.assertEqual(run.call_args.kwargs["cwd"], ROOT)
            self.assertEqual(run.call_args.kwargs["timeout"], 10)

    def test_missing_executable_and_timeout_fail(self):
        for error in (FileNotFoundError("missing"), subprocess.TimeoutExpired("command", 1)):
            with patch("scripts.check.subprocess.run", side_effect=error):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertFalse(check.run_command("failure", ["missing"], 1))


if __name__ == "__main__":
    unittest.main()
