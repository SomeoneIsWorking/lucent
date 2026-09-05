"""Focused fake-process falsifiers for the shipping GTK build/provisioning owners."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import gtk_runtime
from tools.gtk_prerequisites import check_prerequisites


class BuildFixture:
    def __init__(self, root: Path):
        deps = root / "build/deps"
        self.source = deps / "gtk-source"
        self.build = deps / "gtk-build"
        self.prefix = deps / "gtk-install"
        self.source.mkdir(parents=True)
        (self.source / "meson.build").write_text("synthetic fixture", encoding="utf-8")
        self.contract = gtk_runtime.GtkSource("https://example.invalid/gtk.git", "main",
                                               "a" * 40, "b" * 40, "synthetic test")
        self.revision = self.contract.revision
        self.dirty = ""
        self.compiler = "synthetic compiler 1"
        self.targets = "x11 wayland"
        self.selected_prefix = self.prefix
        self.omit_artifact: str | None = None
        self.fail_install = False
        self.calls: list[list[str]] = []

    def run(self, command, **kwargs):
        self.calls.append(command)
        output = ""
        if command[0] == "git":
            output = self.revision if command[3] == "rev-parse" else self.dirty
        elif command[0] == "cc":
            output = self.compiler
        elif command[0] == "pkg-config":
            output = str(self.selected_prefix) if command[1] == "--variable=prefix" else self.targets
        elif command[:3] == list(gtk_runtime.MESON):
            operation = command[3]
            if operation == "--version":
                output = "1.11.1"
            elif operation == "setup":
                coredata = self.build / "meson-private/coredata.dat"
                coredata.parent.mkdir(parents=True, exist_ok=True)
                coredata.write_text("synthetic", encoding="utf-8")
            elif operation == "compile":
                pass
            elif operation == "install":
                if self.fail_install:
                    raise subprocess.CalledProcessError(1, command)
                for relative in (
                    "lib/pkgconfig/gtk+-3.0.pc", "lib/pkgconfig/gdk-3.0.pc",
                    "lib/libgtk-3.so.0", "lib/libgdk-3.so.0", "include/gtk-3.0/gtk/gtk.h",
                    "share/glib-2.0/schemas/org.gtk.Settings.FileChooser.gschema.xml",
                    "share/glib-2.0/schemas/gschemas.compiled",
                ):
                    if relative == self.omit_artifact:
                        continue
                    artifact = self.prefix / relative
                    artifact.parent.mkdir(parents=True, exist_ok=True)
                    if not artifact.exists():
                        artifact.write_text("synthetic", encoding="utf-8")
            else:
                raise AssertionError(f"Unexpected Meson operation: {command}")
        else:
            raise AssertionError(f"Unexpected command: {command}")
        return subprocess.CompletedProcess(command, 0, output, "")

    def provision(self, **kwargs):
        return gtk_runtime.build_gtk(
            self.source, self.build, self.prefix, contract=self.contract,
            environment=kwargs.pop("environment", {}), run=self.run,
            prerequisites=lambda *_args, **_kwargs: {"glib-2.0": "2.88.3"}, **kwargs,
        )

    def meson_calls(self):
        return [command for command in self.calls if command[:3] == list(gtk_runtime.MESON)]


class GtkRuntimeTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "scratch/gtk-runtime-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.fixture = BuildFixture(Path(self.temporary.name))

    def test_immutable_contract_rejects_branch_pins_and_unknown_fields(self):
        path = Path(self.temporary.name) / "gtk.json"
        original = json.loads(gtk_runtime.CONTRACT_PATH.read_text(encoding="utf-8"))
        path.write_text(json.dumps(original), encoding="utf-8")
        self.assertEqual(gtk_runtime.load_contract(path).revision, original["revision"])
        for changed in ({**original, "revision": "main"}, {**original, "unknown": True}):
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaises(RuntimeError):
                gtk_runtime.load_contract(path)

    def test_cold_build_uses_caller_python_native_backends_and_no_tests(self):
        fixture = self.fixture
        self.assertEqual(fixture.provision(), fixture.prefix)
        setup = next(command for command in fixture.meson_calls() if command[3] == "setup")
        self.assertEqual(setup[:3], [sys.executable, "-m", "mesonbuild.mesonmain"])
        for option in ("--libdir=lib", "--backend=ninja", "--wrap-mode=nofallback",
                       "-Dx11_backend=true", "-Dwayland_backend=true", "-Dtests=false"):
            self.assertIn(option, setup)
        compile_command = next(command for command in fixture.meson_calls() if command[3] == "compile")
        self.assertEqual(compile_command[-2:], ["--jobs", "2"])
        self.assertFalse(any(command[3] == "test" for command in fixture.meson_calls()))

    def test_unchanged_build_preserves_manifest_and_uses_incremental_install(self):
        fixture = self.fixture
        fixture.provision()
        manifest = fixture.build / "lucent-gtk-runtime.json"
        before = manifest.stat().st_mtime_ns
        fixture.calls.clear()
        fixture.provision()
        self.assertEqual(manifest.stat().st_mtime_ns, before)
        self.assertFalse(any(command[3] == "setup" for command in fixture.meson_calls()))
        install = next(command for command in fixture.meson_calls() if command[3] == "install")
        self.assertIn("--no-rebuild", install)
        self.assertIn("--only-changed", install)

    def test_dirty_and_wrong_revision_refuse_before_meson(self):
        fixture = self.fixture
        fixture.dirty = " M gtk/gtkfilechooserwidget.c"
        with self.assertRaisesRegex(RuntimeError, "dirty GTK source"):
            fixture.provision()
        self.assertEqual(fixture.meson_calls(), [])
        fixture.dirty = ""
        fixture.revision = "c" * 40
        with self.assertRaisesRegex(RuntimeError, "revision mismatch"):
            fixture.provision()

    def test_toolchain_change_requires_explicit_scoped_rebuild(self):
        fixture = self.fixture
        fixture.provision()
        fixture.compiler = "different compiler"
        fixture.calls.clear()
        with self.assertRaisesRegex(RuntimeError, "incompatible compiler inputs"):
            fixture.provision()
        self.assertFalse(any(command[3] == "setup" for command in fixture.meson_calls()))
        fixture.compiler = "synthetic compiler 1"
        with self.assertRaisesRegex(RuntimeError, "incompatible compiler inputs"):
            fixture.provision(environment={"CFLAGS": "-O0"})

    def test_build_without_manifest_is_not_adopted(self):
        fixture = self.fixture
        coredata = fixture.build / "meson-private/coredata.dat"
        coredata.parent.mkdir(parents=True)
        coredata.write_text("unknown compiler", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "without its compiler/options manifest"):
            fixture.provision()

    def test_failed_install_does_not_return_ready_and_can_resume_owned_configuration(self):
        fixture = self.fixture
        fixture.fail_install = True
        with self.assertRaises(subprocess.CalledProcessError):
            fixture.provision()
        fixture.fail_install = False
        fixture.calls.clear()
        fixture.provision()
        self.assertFalse(any(command[3] == "setup" for command in fixture.meson_calls()))

    def test_prefix_must_have_own_libraries_schemas_and_required_backends(self):
        fixture = self.fixture
        fixture.omit_artifact = "share/glib-2.0/schemas/gschemas.compiled"
        with self.assertRaisesRegex(RuntimeError, "gschemas.compiled"):
            fixture.provision()
        fixture.omit_artifact = None
        fixture.selected_prefix = Path("/usr")
        with self.assertRaisesRegex(RuntimeError, "selected GTK prefix"):
            fixture.provision()
        fixture.selected_prefix = fixture.prefix
        fixture.targets = "x11"
        with self.assertRaisesRegex(RuntimeError, "lacks required X11/Wayland"):
            fixture.provision()

    def test_unscoped_or_overlapping_paths_refuse(self):
        fixture = self.fixture
        with self.assertRaisesRegex(RuntimeError, "distinct siblings"):
            gtk_runtime.validate_paths(fixture.source, fixture.source, fixture.prefix)
        with self.assertRaisesRegex(RuntimeError, "distinct siblings"):
            gtk_runtime.validate_paths(fixture.source, Path(self.temporary.name) / "other", fixture.prefix)


class GtkPrerequisiteTests(unittest.TestCase):
    @staticmethod
    def run_pkg(command, **_kwargs):
        missing = command[1] == "--exists" and command[2].startswith("wayland-protocols")
        return subprocess.CompletedProcess(command, int(missing), "3.0", "")

    def test_fedora_and_debian_name_exact_missing_package_without_installing(self):
        for release, expected in (({"ID": "fedora"}, "sudo dnf install wayland-protocols-devel"),
                                  ({"ID": "ubuntu"}, "sudo apt install wayland-protocols")):
            with self.assertRaisesRegex(RuntimeError, expected):
                check_prerequisites({}, run=self.run_pkg, which=lambda name: name,
                                    release=release, meson_available=True)

    def test_unknown_platform_does_not_guess_package_mapping(self):
        with self.assertRaisesRegex(RuntimeError, "mapping is unknown"):
            check_prerequisites({}, run=self.run_pkg, which=lambda name: name,
                                release={"ID": "unknown"}, meson_available=True)

    def test_meson_must_be_in_exact_caller_environment(self):
        with self.assertRaisesRegex(RuntimeError, "caller's Python environment"):
            check_prerequisites({}, meson_available=False)

    def test_missing_pkgconfig_is_an_actionable_tool_refusal(self):
        with self.assertRaisesRegex(RuntimeError, "sudo dnf install pkgconf-pkg-config"):
            check_prerequisites({}, which=lambda name: None if name == "pkg-config" else name,
                                release={"ID": "fedora"}, meson_available=True)


if __name__ == "__main__":
    unittest.main()
