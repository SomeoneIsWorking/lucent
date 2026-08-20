#!/usr/bin/env python3
"""Refuse new Lucent source monoliths before they become shared infrastructure debt."""

from pathlib import Path
import sys


MAX_LINES = 1000
ROOTS = (Path("include/lucent"), Path("src"), Path("tests"), Path("tools"))
SUFFIXES = {".h", ".cpp", ".py", ".sh"}


def violations(counts: dict[str, int]) -> list[str]:
    return [
        f"{path}: {count} lines exceeds the {MAX_LINES}-line limit"
        for path, count in sorted(counts.items())
        if count > MAX_LINES
    ]


def selftest() -> int:
    assert not violations({"boundary.cpp": MAX_LINES})
    result = violations({"grown.cpp": MAX_LINES + 1})
    assert result == ["grown.cpp: 1001 lines exceeds the 1000-line limit"]
    print("source structure self-test passed")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:]:
        print("usage: tools/check_source_structure.py [--selftest]", file=sys.stderr)
        return 2

    counts: dict[str, int] = {}
    for root in ROOTS:
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in SUFFIXES:
                counts[str(path)] = len(path.read_text(encoding="utf-8").splitlines())
    failed = violations(counts)
    if failed:
        print("source structure check failed:", file=sys.stderr)
        for message in failed:
            print(f"  {message}", file=sys.stderr)
        return 1
    print(f"source structure check passed: {len(counts)} files, limit {MAX_LINES}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
