#!/usr/bin/env python3
"""Run Lucent's non-mutating format and clang-tidy checks."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (ROOT / "include/lucent", ROOT / "src", ROOT / "tests")


def require_tool(name: str, variable: str) -> str:
    path = os.environ.get(variable) or shutil.which(name)
    if not path:
        raise SystemExit(f"REFUSING: {name} is not installed")
    return path


def source_files(suffixes: set[str]) -> list[str]:
    return sorted(
        str(path)
        for root in SOURCE_ROOTS
        for path in root.rglob("*")
        if path.is_file() and path.suffix in suffixes
    )


def run(command: list[str]) -> None:
    subprocess.run(command, cwd=ROOT, check=True)


def resolve_build_dir(argument: str | None) -> Path:
    build_dir = Path(argument) if argument is not None else ROOT / "build"
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    return build_dir.resolve()


def configured_translation_units(
    compile_commands: Path, first_party: set[Path] | None = None
) -> list[str]:
    if not compile_commands.is_file():
        raise SystemExit(f"REFUSING: {compile_commands} is missing")
    try:
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(
            f"REFUSING: cannot read {compile_commands}: {error}"
        ) from error
    if not isinstance(entries, list):
        raise SystemExit(
            f"REFUSING: {compile_commands} is not a compilation database array"
        )

    configured: set[Path] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise SystemExit(
                f"REFUSING: {compile_commands} entry {index} has no source file"
            )
        source = Path(entry["file"])
        if not source.is_absolute():
            directory = entry.get("directory")
            if not isinstance(directory, str):
                raise SystemExit(
                    f"REFUSING: {compile_commands} entry {index} has no build directory"
                )
            source = Path(directory) / source
        configured.add(source.resolve())

    if first_party is None:
        first_party = {Path(source).resolve() for source in source_files({".cpp"})}
    selected = sorted(str(source) for source in first_party & configured)
    if not selected:
        raise SystemExit(
            f"REFUSING: {compile_commands} scanned {len(entries)} entries and contains no "
            "configured first-party C++ sources"
        )
    return selected


def main() -> int:
    if len(sys.argv) > 2:
        raise SystemExit("usage: tools/check_cpp_quality.py [build-directory]")
    build_dir = resolve_build_dir(sys.argv[1] if len(sys.argv) == 2 else None)
    compile_commands = build_dir / "compile_commands.json"

    formatter = require_tool("clang-format", "CLANG_FORMAT")
    tidy = require_tool("clang-tidy", "CLANG_TIDY")
    clang = require_tool("clang++", "CLANG_CXX")
    headers_and_sources = source_files({".h", ".cpp"})
    translation_units = configured_translation_units(compile_commands)
    run([formatter, "--dry-run", "--Werror", *headers_and_sources])
    resource_dir = subprocess.check_output(
        [clang, "-print-resource-dir"], cwd=ROOT, text=True
    ).strip()
    run(
        [
            tidy,
            "-p",
            str(build_dir),
            *translation_units,
            f"--extra-arg=-resource-dir={resource_dir}",
            "--quiet",
        ]
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
