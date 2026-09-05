"""Named Linux prerequisites for Lucent's optional GTK runtime build."""

from __future__ import annotations

from dataclasses import dataclass
import importlib.util
import platform
import shlex
import shutil
import subprocess
from typing import Callable, Mapping

Run = Callable[..., subprocess.CompletedProcess[str]]


@dataclass(frozen=True)
class Dependency:
    expression: str
    fedora: str
    debian: str


DEPENDENCIES = (
    Dependency("glib-2.0 >= 2.57.2", "glib2-devel", "libglib2.0-dev"),
    Dependency("gio-unix-2.0 >= 2.57.2", "glib2-devel", "libglib2.0-dev"),
    Dependency("cairo >= 1.14.0", "cairo-devel", "libcairo2-dev"),
    Dependency("cairo-gobject >= 1.14.0", "cairo-gobject-devel", "libcairo2-dev"),
    Dependency("cairo-xlib >= 1.14.0", "cairo-devel", "libcairo2-dev"),
    Dependency("pango >= 1.41.0", "pango-devel", "libpango1.0-dev"),
    Dependency("pangoft2", "pango-devel", "libpango1.0-dev"),
    Dependency("pangocairo", "pango-devel", "libpango1.0-dev"),
    Dependency("fribidi >= 0.19.7", "fribidi-devel", "libfribidi-dev"),
    Dependency("freetype2 >= 2.7.1", "freetype-devel", "libfreetype-dev"),
    Dependency("fontconfig", "fontconfig-devel", "libfontconfig-dev"),
    Dependency("gdk-pixbuf-2.0 >= 2.30.0", "gdk-pixbuf2-devel", "libgdk-pixbuf-2.0-dev"),
    Dependency("epoxy >= 1.4", "libepoxy-devel", "libepoxy-dev"),
    Dependency("atk >= 2.35.1", "atk-devel", "libatk1.0-dev"),
    Dependency("atk-bridge-2.0 >= 2.15.1", "at-spi2-atk-devel", "libatk-bridge2.0-dev"),
    Dependency("harfbuzz >= 0.9", "harfbuzz-devel", "libharfbuzz-dev"),
    Dependency("xkbcommon >= 0.2.0", "libxkbcommon-devel", "libxkbcommon-dev"),
    Dependency("wayland-client >= 1.14.91", "wayland-devel", "libwayland-dev"),
    Dependency("wayland-cursor >= 1.14.91", "wayland-devel", "libwayland-dev"),
    Dependency("wayland-egl", "wayland-devel", "libwayland-dev"),
    Dependency("wayland-protocols >= 1.17", "wayland-protocols-devel", "wayland-protocols"),
    Dependency("xrandr >= 1.2.99", "libXrandr-devel", "libxrandr-dev"),
    Dependency("x11", "libX11-devel", "libx11-dev"),
    Dependency("xrender", "libXrender-devel", "libxrender-dev"),
    Dependency("xi", "libXi-devel", "libxi-dev"),
    Dependency("xext", "libXext-devel", "libxext-dev"),
)

TOOLS = {
    "git": ("git", "git"),
    "ninja": ("ninja-build", "ninja-build"),
    "pkg-config": ("pkgconf-pkg-config", "pkg-config"),
    "msgfmt": ("gettext", "gettext"),
    "glib-compile-resources": ("glib2-devel", "libglib2.0-dev-bin"),
    "glib-mkenums": ("glib2-devel", "libglib2.0-dev-bin"),
    "glib-compile-schemas": ("glib2", "libglib2.0-bin"),
    "wayland-scanner": ("wayland-devel", "libwayland-bin"),
}


def distro_family(release: Mapping[str, str]) -> str:
    names = {release.get("ID", ""), *release.get("ID_LIKE", "").split()}
    if names & {"fedora", "rhel", "centos"}:
        return "fedora"
    if names & {"debian", "ubuntu"}:
        return "debian"
    raise RuntimeError(
        "GTK prerequisite mapping is unknown for this distribution; provide the supported "
        "Linux distribution/version and package path before installing dependencies"
    )


def check_prerequisites(
    environment: Mapping[str, str],
    *,
    run: Run = subprocess.run,
    which: Callable[[str], str | None] = shutil.which,
    release: Mapping[str, str] | None = None,
    meson_available: bool | None = None,
) -> dict[str, str]:
    """Refuse missing inputs by exact name; never install or substitute dependencies."""
    if meson_available is None:
        meson_available = importlib.util.find_spec("mesonbuild") is not None
    if not meson_available:
        raise RuntimeError(
            "Meson is missing from the caller's Python environment; add meson to the "
            "consumer's locked dependencies and run uv sync --frozen"
        )
    missing: list[str] = []
    package_pairs: list[tuple[str, str]] = []
    for tool, packages in TOOLS.items():
        if which(tool) is None:
            missing.append(tool)
            package_pairs.append(packages)
    compiler = shlex.split(environment.get("CC", "cc"))
    if not compiler:
        raise RuntimeError("CC is empty; select an installed C compiler")
    if which(compiler[0]) is None:
        compiler_packages = {
            "cc": ("gcc", "build-essential"),
            "gcc": ("gcc", "gcc"),
            "clang": ("clang", "clang"),
            "ccache": ("ccache", "ccache"),
        }.get(compiler[0])
        if compiler_packages is None:
            raise RuntimeError(f"Selected C compiler command is missing: {compiler[0]}")
        missing.append(compiler[0])
        package_pairs.append(compiler_packages)
    versions: dict[str, str] = {}
    if which("pkg-config") is not None:
        for dependency in DEPENDENCIES:
            present = run(
                ["pkg-config", "--exists", dependency.expression],
                env=dict(environment), capture_output=True, text=True, check=False,
            )
            if present.returncode:
                missing.append(dependency.expression)
                package_pairs.append((dependency.fedora, dependency.debian))
            else:
                package = dependency.expression.split()[0]
                result = run(
                    ["pkg-config", "--modversion", package],
                    env=dict(environment), capture_output=True, text=True, check=True,
                )
                versions[package] = result.stdout.strip()
    if missing:
        family = distro_family(platform.freedesktop_os_release() if release is None else release)
        index = 0 if family == "fedora" else 1
        packages = sorted({pair[index] for pair in package_pairs})
        manager = "dnf" if family == "fedora" else "apt"
        raise RuntimeError(
            f"Missing GTK prerequisites: {', '.join(missing)}. Run yourself: "
            f"sudo {manager} install {' '.join(packages)}"
        )
    return versions
