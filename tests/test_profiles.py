"""Profile generation and failure-path tests; compiler availability is not assumed."""

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import tomllib

from scripts.common import (
    REQUIRED_FILES,
    ROOT,
    baseline_errors,
    load_config,
    validate_config,
    write_text,
)
from scripts.init_project import initialize
from scripts.profiles import LANGUAGES, confined_path


def new_config():
    """Uninitialized config mirroring the shipped template, version included."""
    return {
        "schema_version": 1,
        "template_version": load_config(ROOT)["template_version"],
        "initialized": False,
        "language": "generic",
        "name": None,
        "description": None,
        "stack": None,
        "quality_commands": [],
    }


class ProfileTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="language-profile-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        for filename in REQUIRED_FILES:
            path = self.root / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            write_text(path, "")
        shutil.copytree(ROOT / "profiles", self.root / "profiles")
        write_text(self.root / "project.json", json.dumps(new_config()))
        write_text(
            self.root / "docs/project-context.md",
            "{{PROJECT_NAME}}\n{{PROJECT_DESCRIPTION}}\n{{PROJECT_STACK}}\n",
        )

    def initialize(self, language, **kwargs):
        return initialize(
            self.root,
            "sample-project",
            "Sample goal",
            "Test stack",
            language=language,
            **kwargs,
        )

    def manifest(self, language):
        path = self.root / "profiles" / language / "profile.json"
        return path, json.loads(path.read_text(encoding="utf-8"))

    def test_generates_each_profile_without_other_language_assets(self):
        for language in LANGUAGES[1:]:
            with self.subTest(language=language):
                # Preview all profiles in the same still-uninitialized fixture.
                paths = self.initialize(language, dry_run=True)
                self.assertGreater(len(paths), 2)
                self.assertFalse(load_config(self.root)["initialized"])
        paths = self.initialize("cpp")
        self.assertTrue((self.root / "CMakeLists.txt").exists())
        self.assertFalse((self.root / "package.json").exists())
        self.assertFalse((self.root / "app/__init__.py").exists())
        self.assertEqual(load_config(self.root)["language"], "cpp")
        self.assertEqual(baseline_errors(self.root, strict=True), [])
        for name in paths:
            self.assertNotIn(
                "@@PROJECT_NAME@@", (self.root / name).read_text(encoding="utf-8")
            )

    def test_dry_run_preserves_files_and_directories(self):
        before = {
            str(p.relative_to(self.root)): p.read_bytes() if p.is_file() else None
            for p in self.root.rglob("*")
        }
        self.initialize("cpp", dry_run=True)
        after = {
            str(p.relative_to(self.root)): p.read_bytes() if p.is_file() else None
            for p in self.root.rglob("*")
        }
        self.assertEqual(before, after)

    def test_existing_destination_is_not_overwritten(self):
        write_text(self.root / "CMakeLists.txt", "user content")
        with self.assertRaisesRegex(ValueError, "overwrite"):
            self.initialize("cpp")
        self.assertEqual((self.root / "CMakeLists.txt").read_text(), "user content")
        self.assertFalse(load_config(self.root)["initialized"])

    def test_existing_project_workflow_is_not_overwritten(self):
        path = self.root / ".github/workflows/project.yml"
        path.parent.mkdir(parents=True, exist_ok=True)
        write_text(path, "existing workflow")
        with self.assertRaisesRegex(ValueError, "overwrite"):
            self.initialize("python")
        self.assertFalse((self.root / "app").exists())

    def test_existing_quality_commands_are_not_replaced(self):
        config = new_config()
        config["quality_commands"] = [{"name": "existing", "argv": ["tool"]}]
        write_text(self.root / "project.json", json.dumps(config))
        with self.assertRaisesRegex(ValueError, "quality_commands"):
            self.initialize("cpp")
        self.assertFalse((self.root / "CMakeLists.txt").exists())

    def test_unknown_language_and_legacy_config(self):
        with self.assertRaises(ValueError):
            self.initialize("unsupported-language")
        with self.assertRaises(ValueError):
            validate_config({**new_config(), "language": "unsupported-language"})
        config = new_config()
        del config["language"]
        validate_config(config)  # 0.1 configurations remain readable.

    def test_path_escape_and_absolute_paths_rejected(self):
        for path in (
            "../escape",
            "/absolute",
            "C:/absolute",
            "bad\\path",
            ".",
            "a/../b",
            "a//b",
        ):
            with self.subTest(path=path), self.assertRaises(ValueError):
                confined_path(self.root, path)

    def test_manifest_escape_rejected_before_writes(self):
        path, manifest = self.manifest("cpp")
        manifest["files"]["../escape.cpp"] = "files/project.cpp.in"
        write_text(path, json.dumps(manifest))
        with self.assertRaises(ValueError):
            self.initialize("cpp")
        self.assertFalse(load_config(self.root)["initialized"])

    def test_invalid_manifest_commands_rejected_before_writes(self):
        path, manifest = self.manifest("cpp")
        manifest["quality_commands"] = "not an array"
        write_text(path, json.dumps(manifest))
        with self.assertRaises(ValueError):
            self.initialize("cpp")
        self.assertFalse((self.root / "CMakeLists.txt").exists())

    def test_new_files_and_directories_rolled_back_after_failure(self):
        before = {
            p: p.read_bytes() if p.is_file() else None for p in self.root.rglob("*")
        }

        def fail_at_header(path, text):
            if path.name == "project.hpp":
                raise OSError("simulated scaffold write failure")
            write_text(path, text)

        with patch("scripts.init_project.write_text", side_effect=fail_at_header):
            with self.assertRaisesRegex(OSError, "simulated"):
                self.initialize("cpp")
        after = {
            p: p.read_bytes() if p.is_file() else None for p in self.root.rglob("*")
        }
        self.assertEqual(before, after)

    def test_cpp_presets_refer_to_real_configure_presets(self):
        self.initialize("cpp")
        presets = json.loads((self.root / "CMakePresets.json").read_text())
        configure = {item["name"] for item in presets["configurePresets"]}
        for item in presets["buildPresets"] + presets["testPresets"]:
            self.assertIn(item["configurePreset"], configure)
        source = (self.root / "tests/cpp/project_test.cpp").read_text()
        self.assertNotIn("assert(", source)
        self.assertIn("EXIT_FAILURE", source)

    def test_cpp_pipeline_stops_after_configure_failure(self):
        self.initialize("cpp")
        module = self.load_generated(
            "generated_cpp_check", self.root / "scripts/cpp_check.py"
        )
        with patch.object(sys, "argv", ["cpp_check.py", "--preset", "dev"]):
            with patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 2),
            ) as run:
                self.assertEqual(module.main(), 2)
                self.assertEqual(run.call_count, 1)

    def test_python_starter_tests_really_run(self):
        self.initialize("python")
        result = subprocess.run(
            [sys.executable, "-m", "unittest", "discover", "-s", "app_tests", "-v"],
            cwd=self.root,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Ran 2 tests", result.stderr)
        self.assertEqual(baseline_errors(self.root, strict=True), [])

    def test_typescript_strict_config_and_no_fabricated_lock(self):
        self.initialize("typescript")
        config = json.loads((self.root / "tsconfig.json").read_text())
        self.assertTrue(config["compilerOptions"]["strict"])
        self.assertTrue(config["compilerOptions"]["noEmitOnError"])
        self.assertFalse((self.root / "package-lock.json").exists())
        self.assertIn(
            "npm ci --ignore-scripts",
            (self.root / ".github/workflows/project.yml").read_text(),
        )
        self.assertEqual(baseline_errors(self.root, strict=True), [])

    @unittest.skipUnless(
        shutil.which("clang-format"), "optional clang-format is not installed"
    )
    def test_generated_cpp_matches_its_format_rules(self):
        self.initialize("cpp")
        result = subprocess.run(
            [sys.executable, "scripts/cpp_format.py"],
            cwd=self.root,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(
        shutil.which("clang-format"), "optional clang-format is not installed"
    )
    def test_longest_project_name_is_also_clang_formatted(self):
        initialize(self.root, "x" * 64, "Sample goal", "C++17", language="cpp")
        result = subprocess.run(
            [sys.executable, "scripts/cpp_format.py"],
            cwd=self.root,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_linux_compiler_presets_are_explicit_and_isolated(self):
        self.initialize("cpp")
        data = json.loads((self.root / "CMakePresets.json").read_text())
        presets = {item["name"]: item for item in data["configurePresets"]}
        names = (
            "linux-gcc-debug",
            "linux-gcc-release",
            "linux-clang-debug",
            "linux-clang-release",
        )
        self.assertEqual(len({presets[name]["binaryDir"] for name in names}), 4)
        for family, compiler in (("gcc", "g++"), ("clang", "clang++")):
            debug = presets[f"linux-{family}-debug"]
            release = presets[f"linux-{family}-release"]
            self.assertEqual(debug["cacheVariables"]["CMAKE_CXX_COMPILER"], compiler)
            self.assertEqual(debug["condition"]["rhs"], "Linux")
            self.assertEqual(release["inherits"], debug["name"])
            self.assertEqual(release["cacheVariables"]["CMAKE_BUILD_TYPE"], "Release")
        for name in names:
            self.assertIn(name, {item["name"] for item in data["buildPresets"]})
            self.assertIn(name, {item["name"] for item in data["testPresets"]})
            self.assertIn(
                name, (self.root / ".github/workflows/project.yml").read_text()
            )

    def test_linux_default_and_clang_override(self):
        module = self.generated_driver("cpp")
        with patch.object(module.sys, "platform", "linux"):
            self.assertEqual(module.default_preset(), "linux-gcc-debug")
        with patch.object(
            sys, "argv", ["cpp_check.py", "--preset", "linux-clang-release"]
        ):
            with patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as run:
                self.assertEqual(module.main(), 0)
                self.assertEqual(
                    run.call_args_list[0].args[0][2], "linux-clang-release"
                )
                self.assertIn("--no-tests=error", run.call_args_list[-1].args[0])

    def test_non_linux_default_preset_is_dev(self):
        module = self.generated_driver("cpp")
        for platform in ("darwin", "freebsd14"):
            with patch.object(module.sys, "platform", platform):
                self.assertEqual(module.default_preset(), "dev", platform)
        self.assertNotIn("windows-msvc", module.PRESETS)
        self.assertEqual(
            sorted(module.PRESETS),
            [
                "dev",
                "linux-clang-debug",
                "linux-clang-release",
                "linux-gcc-debug",
                "linux-gcc-release",
                "release",
            ],
        )

    def test_cpp_profile_has_no_windows_or_msvc_paths(self):
        self.initialize("cpp")
        data = json.loads((self.root / "CMakePresets.json").read_text())
        names = {
            item["name"]
            for section in ("configurePresets", "buildPresets", "testPresets")
            for item in data[section]
        }
        self.assertEqual(len(names), 6)
        self.assertFalse(
            [name for name in names if "windows" in name or "msvc" in name]
        )
        raw = json.dumps(data)
        self.assertNotIn("Visual Studio", raw)
        for filename in ("CMakeLists.txt", "scripts/cpp_check.py"):
            source = (self.root / filename).read_text()
            lowered = source.lower()
            self.assertNotIn("msvc", lowered, filename)
            self.assertNotIn("windows", lowered, filename)
            self.assertNotIn("/W4", source, filename)

    def test_generated_ci_targets_linux_only(self):
        for language in ("cpp", "python", "typescript", "go", "rust"):
            with self.subTest(language=language):
                root = Path(tempfile.mkdtemp(prefix=f"ci-matrix-{language}-"))
                self.addCleanup(shutil.rmtree, root, True)
                for filename in REQUIRED_FILES:
                    path = root / filename
                    path.parent.mkdir(parents=True, exist_ok=True)
                    write_text(path, "")
                shutil.copytree(ROOT / "profiles", root / "profiles")
                write_text(root / "project.json", json.dumps(new_config()))
                write_text(
                    root / "docs/project-context.md",
                    "{{PROJECT_NAME}}\n{{PROJECT_DESCRIPTION}}\n{{PROJECT_STACK}}\n",
                )
                initialize(
                    root,
                    "sample-project",
                    "Sample goal",
                    "Sample stack",
                    language=language,
                )
                workflow = (root / ".github/workflows/project.yml").read_text().lower()
                self.assertNotIn("windows", workflow)
                self.assertIn("ubuntu", workflow)

    def test_template_ci_targets_linux_only(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text().lower()
        self.assertNotIn("windows", workflow)
        self.assertIn("ubuntu-latest", workflow)

    def test_go_scaffold_module_imports_and_isolation(self):
        self.initialize("go")
        self.assertIn(
            "module example.com/sample-project", (self.root / "go.mod").read_text()
        )
        self.assertIn(
            '"example.com/sample-project/internal/project"',
            (self.root / "cmd/starter/main.go").read_text(),
        )
        self.assertFalse((self.root / "Cargo.toml").exists())
        self.assertFalse((self.root / "CMakeLists.txt").exists())
        self.assertFalse(
            (self.root / "go.sum").exists()
        )  # No external modules were resolved.
        self.assertEqual(load_config(self.root)["language"], "go")
        self.assertEqual(baseline_errors(self.root, strict=True), [])

    def test_rust_scaffold_manifest_lock_and_isolation(self):
        # Repository slugs need not be valid Rust crate identifiers.
        initialize(self.root, "123-project", "Goal", "Rust", language="rust")
        manifest = tomllib.loads((self.root / "Cargo.toml").read_text())
        lock = tomllib.loads((self.root / "Cargo.lock").read_text())
        self.assertEqual(manifest["package"]["edition"], "2024")
        self.assertFalse(manifest["package"]["publish"])
        self.assertEqual(manifest["dependencies"], {})
        self.assertEqual(
            lock["package"],
            [
                {
                    "name": manifest["package"]["name"],
                    "version": manifest["package"]["version"],
                }
            ],
        )
        self.assertEqual(manifest["lints"]["rust"]["unsafe_code"], "forbid")
        self.assertIn('"123-project"', (self.root / "src/lib.rs").read_text())
        self.assertFalse((self.root / "go.mod").exists())
        self.assertEqual(baseline_errors(self.root, strict=True), [])

    def test_go_and_rust_refuse_existing_manifests(self):
        for language, filename in (("go", "go.mod"), ("rust", "Cargo.toml")):
            with self.subTest(language=language):
                path = self.root / filename
                write_text(path, "user content")
                with self.assertRaisesRegex(ValueError, "overwrite"):
                    self.initialize(language)
                self.assertEqual(path.read_text(), "user content")
                path.unlink()
        self.assertFalse(load_config(self.root)["initialized"])

    @staticmethod
    def load_generated(name, path):
        """Import a generated check script, failing loudly when it cannot be loaded."""
        spec = importlib.util.spec_from_file_location(name, path)
        if spec is None or spec.loader is None:
            raise AssertionError(f"cannot load generated module {name} from {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def generated_driver(self, language):
        self.initialize(language)
        return self.load_generated(
            f"generated_{language}_check", self.root / f"scripts/{language}_check.py"
        )

    def test_go_unformatted_output_fails_even_with_zero_exit(self):
        module = self.generated_driver("go")
        with patch.object(sys, "argv", ["go_check.py"]):
            with patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    [], 0, "internal/project/name.go\n", ""
                ),
            ) as run:
                self.assertEqual(module.main(), 1)
                self.assertEqual(run.call_count, 1)

    def test_go_pipeline_offline_readonly_and_optional_race(self):
        module = self.generated_driver("go")
        with patch.object(sys, "argv", ["go_check.py", "--race"]):
            with patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, "", ""),
            ) as run:
                self.assertEqual(module.main(), 0)
                calls = run.call_args_list
                self.assertEqual(len(calls), 4)
                self.assertIn("-race", calls[2].args[0])
                for call in calls:
                    self.assertFalse(call.kwargs["shell"])
                    self.assertEqual(call.kwargs["env"]["GOTOOLCHAIN"], "local")
                    self.assertEqual(call.kwargs["env"]["GOPROXY"], "off")
                    self.assertEqual(call.kwargs["env"]["GOWORK"], "off")
                for call in calls[1:]:
                    self.assertIn("-mod=readonly", call.args[0])

    def test_go_pipeline_stops_after_vet_failure(self):
        module = self.generated_driver("go")
        with patch.object(sys, "argv", ["go_check.py"]):
            with patch.object(
                module.subprocess,
                "run",
                side_effect=[
                    subprocess.CompletedProcess([], 0, "", ""),
                    subprocess.CompletedProcess([], 2),
                ],
            ) as run:
                self.assertEqual(module.main(), 2)
                self.assertEqual(run.call_count, 2)

    def test_rust_missing_lockfile_fails_before_running_cargo(self):
        module = self.generated_driver("rust")
        (self.root / "Cargo.lock").unlink()
        with patch.object(sys, "argv", ["rust_check.py"]):
            with patch.object(module.subprocess, "run") as run:
                self.assertEqual(module.main(), 1)
                run.assert_not_called()

    def test_rust_pipeline_stops_after_format_failure(self):
        module = self.generated_driver("rust")
        with patch.object(sys, "argv", ["rust_check.py"]):
            with patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 1),
            ) as run:
                self.assertEqual(module.main(), 1)
                self.assertEqual(run.call_count, 1)

    def test_rust_offline_locked_release_and_doctest_command(self):
        module = self.generated_driver("rust")
        with patch.object(sys, "argv", ["rust_check.py", "--release"]):
            with patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as run:
                self.assertEqual(module.main(), 0)
                calls = run.call_args_list
                self.assertEqual(len(calls), 4)
                for call in calls:
                    self.assertFalse(call.kwargs["shell"])
                    self.assertEqual(call.kwargs["env"]["CARGO_NET_OFFLINE"], "true")
                    self.assertEqual(call.kwargs["env"]["RUSTUP_AUTO_INSTALL"], "0")
                for call in calls[1:]:
                    for flag in ("--offline", "--locked", "--release"):
                        self.assertIn(flag, call.args[0])
                self.assertNotIn(
                    "--all-targets", calls[2].args[0]
                )  # Preserve doc tests.

    def test_generated_python_scripts_have_valid_syntax(self):
        for language in LANGUAGES[1:]:
            _, manifest = self.manifest(language)
            for destination, source in manifest["files"].items():
                if destination.endswith(".py"):
                    path = self.root / "profiles" / language / source
                    compile(
                        path.read_text(encoding="utf-8").replace(
                            "@@PROJECT_NAME@@", "sample"
                        ),
                        str(path),
                        "exec",
                    )


if __name__ == "__main__":
    unittest.main()
