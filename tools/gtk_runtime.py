#!/usr/bin/env python3
"""Build the pinned optional GTK fork into a caller-owned native dependency prefix."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Callable, Mapping

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.gtk_prerequisites import Run, check_prerequisites

CONTRACT_PATH = Path(__file__).resolve().parent.parent / "dependencies/gtk.json"
MESON = (sys.executable, "-m", "mesonbuild.mesonmain")
OPTIONS = (
    "--backend=ninja", "--buildtype=release", "--libdir=lib", "--wrap-mode=nofallback",
    "-Ddefault_library=shared", "-Dx11_backend=true", "-Dwayland_backend=true",
    "-Dbroadway_backend=false", "-Dwin32_backend=false", "-Dquartz_backend=false",
    "-Dtests=false", "-Dinstalled_tests=false", "-Ddemos=false", "-Dexamples=false",
    "-Dgtk_doc=false", "-Dman=false", "-Dintrospection=false", "-Dprint_backends=file",
    "-Dcolord=no", "-Dcloudproviders=false", "-Dprofiler=false", "-Dtracker3=false",
)
ENVIRONMENT_INPUTS = (
    "CC", "CXX", "AR", "CFLAGS", "CPPFLAGS", "LDFLAGS", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR",
)
TOOLCHAIN_INPUTS = ("CC", "CXX", "AR", "CFLAGS", "CPPFLAGS", "LDFLAGS")


@dataclass(frozen=True)
class GtkSource:
    repository: str
    ref: str
    revision: str
    upstream_base: str
    purpose: str


def load_contract(path: Path = CONTRACT_PATH) -> GtkSource:
    value = json.loads(path.read_text(encoding="utf-8"))
    fields = {"schemaVersion", "repository", "ref", "revision", "upstreamBase", "purpose"}
    if not isinstance(value, dict) or set(value) != fields or value["schemaVersion"] != 1:
        raise RuntimeError(f"Invalid GTK source contract schema: {path}")
    if any(not isinstance(value[key], str) or not value[key] for key in fields - {"schemaVersion"}):
        raise RuntimeError(f"GTK source contract contains empty/non-string fields: {path}")
    for name in ("revision", "upstreamBase"):
        if re.fullmatch(r"[0-9a-f]{40}", value[name]) is None:
            raise RuntimeError(f"GTK {name} must be an immutable 40-character commit: {path}")
    return GtkSource(value["repository"], value["ref"], value["revision"],
                     value["upstreamBase"], value["purpose"])


def validate_paths(source: Path, build: Path, prefix: Path) -> tuple[Path, Path, Path]:
    paths = tuple(path.resolve() for path in (source, build, prefix))
    parent = paths[0].parent
    if (len(set(paths)) != 3 or any(path.parent != parent for path in paths)
            or parent.name != "deps" or parent.parent.name != "build"):
        raise RuntimeError("GTK source, build, and prefix must be distinct siblings under caller build/deps")
    return paths


def validate_source(source: Path, contract: GtkSource, run: Run) -> None:
    if not (source / "meson.build").is_file():
        raise RuntimeError(f"GTK source is missing meson.build: {source}")
    revision = run(["git", "-C", str(source), "rev-parse", "HEAD"],
                   capture_output=True, text=True, check=True).stdout.strip()
    if revision != contract.revision:
        raise RuntimeError(f"GTK revision mismatch: expected {contract.revision}, observed {revision}")
    status = run(["git", "-C", str(source), "status", "--porcelain=v1", "--untracked-files=all"],
                 capture_output=True, text=True, check=True).stdout.strip()
    if status:
        raise RuntimeError(f"Refusing dirty GTK source checkout: {source}\n{status}")


def validate_prefix(prefix: Path, environment: Mapping[str, str], run: Run) -> None:
    for relative in (
        "lib/pkgconfig/gtk+-3.0.pc", "lib/pkgconfig/gdk-3.0.pc",
        "lib/libgtk-3.so.0", "lib/libgdk-3.so.0", "include/gtk-3.0/gtk/gtk.h",
        "share/glib-2.0/schemas/org.gtk.Settings.FileChooser.gschema.xml",
        "share/glib-2.0/schemas/gschemas.compiled",
    ):
        candidate = prefix / relative
        if not candidate.is_file() or not candidate.resolve().is_relative_to(prefix):
            raise RuntimeError(f"GTK prefix is missing its own installed artifact: {candidate}")
    selected_environment = dict(environment)
    selected_environment["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig") + (
        os.pathsep + environment["PKG_CONFIG_PATH"] if environment.get("PKG_CONFIG_PATH") else ""
    )
    observed = run(["pkg-config", "--variable=prefix", "gtk+-3.0"],
                   env=selected_environment, capture_output=True, text=True, check=True).stdout.strip()
    if Path(observed).resolve() != prefix:
        raise RuntimeError(f"pkg-config selected GTK prefix {observed}, expected {prefix}")
    targets = run(["pkg-config", "--variable=targets", "gtk+-3.0"],
                  env=selected_environment, capture_output=True, text=True, check=True).stdout.split()
    if not {"x11", "wayland"}.issubset(targets):
        raise RuntimeError(f"Installed GTK lacks required X11/Wayland backends: {targets}")


def build_gtk(
    source: Path, build: Path, prefix: Path, *, contract: GtkSource | None = None,
    environment: Mapping[str, str] | None = None, run: Run = subprocess.run,
    prerequisites: Callable[..., dict[str, str]] = check_prerequisites,
) -> Path:
    if sys.platform != "linux":
        raise RuntimeError("Lucent GTK fork provisioning currently supports Linux only")
    contract = load_contract() if contract is None else contract
    source, build, prefix = validate_paths(source, build, prefix)
    environment = dict(os.environ if environment is None else environment)
    versions = prerequisites(environment, run=run)
    validate_source(source, contract, run)
    compiler = shlex.split(environment.get("CC", "cc"))
    compiler_version = run([*compiler, "--version"], env=environment,
                           capture_output=True, text=True, check=True).stdout.strip()
    meson_version = run([*MESON, "--version"], env=environment,
                        capture_output=True, text=True, check=True).stdout.strip()
    identity = {
        "schemaVersion": 1, "repository": contract.repository, "revision": contract.revision,
        "source": str(source), "prefix": str(prefix), "options": list(OPTIONS),
        "compilerVersion": compiler_version, "mesonVersion": meson_version,
        "python": sys.executable, "dependencies": versions,
        "environment": {name: environment.get(name) for name in ENVIRONMENT_INPUTS},
    }
    manifest = build / "lucent-gtk-runtime.json"
    previous = json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else None
    if previous is not None and not isinstance(previous, dict):
        raise RuntimeError(f"Invalid GTK build manifest: {manifest}")
    configured = (build / "meson-private/coredata.dat").is_file()
    if build.exists() and any(build.iterdir()) and not configured:
        raise RuntimeError(f"Refusing non-Meson GTK build directory: {build}")
    if configured and previous is None:
        raise RuntimeError(f"Refusing GTK build without its compiler/options manifest: {build}")
    if configured and previous is not None and previous.get("source") != str(source):
        raise RuntimeError(f"GTK build belongs to a different source path: {build}")
    if configured and previous is not None and (
        previous.get("compilerVersion") != compiler_version
        or any(previous.get("environment", {}).get(name) != environment.get(name)
               for name in TOOLCHAIN_INPUTS)
    ):
        raise RuntimeError(
            f"GTK build has incompatible compiler inputs: {build}; remove this exact generated "
            "build directory before rebuilding with the selected compiler"
        )
    if previous != identity or not configured:
        run([*MESON, "setup", *( ["--reconfigure", "--clearcache"] if configured else [] ), str(build),
             str(source), f"--prefix={prefix}", *OPTIONS], env=environment, check=True)
        # This records configured inputs, not install readiness. A failed compile/install
        # resumes through the same owners on the next invocation and never prints ready.
        manifest.write_text(json.dumps(identity, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    run([*MESON, "compile", "-C", str(build), "--jobs", "2"], env=environment, check=True)
    run([*MESON, "install", "-C", str(build), "--no-rebuild", "--only-changed"], env=environment, check=True)
    validate_prefix(prefix, environment, run)
    validate_source(source, contract, run)
    return prefix


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--prefix", required=True, type=Path)
    options = parser.parse_args()
    try:
        prefix = build_gtk(options.source, options.build, options.prefix)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"REFUSING GTK runtime provisioning: {error}", file=sys.stderr)
        return 1
    print(f"GTK runtime prefix ready: {prefix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
