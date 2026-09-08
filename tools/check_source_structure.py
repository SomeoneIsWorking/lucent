#!/usr/bin/env python3
"""Refuse new Lucent source monoliths before they become shared infrastructure debt."""

from pathlib import Path
import sys


MAX_LINES = 1000
REPOSITORY = Path(__file__).resolve().parents[1]
ROOTS = (Path("include/lucent"), Path("src"), Path("tests"), Path("tools"))
SUFFIXES = {".h", ".cpp", ".py", ".sh"}


def violations(counts: dict[str, int]) -> list[str]:
    return [
        f"{path}: {count} lines exceeds the {MAX_LINES}-line limit"
        for path, count in sorted(counts.items())
        if count > MAX_LINES
    ]


def source_counts(repository: Path) -> dict[str, int]:
    missing = [str(root) for root in ROOTS if not (repository / root).is_dir()]
    if missing:
        raise ValueError(f"missing source directories under {repository}: {', '.join(missing)}")
    counts = {
        str(path.relative_to(repository)): len(path.read_text(encoding="utf-8").splitlines())
        for root in ROOTS
        for path in (repository / root).rglob("*")
        if path.is_file() and path.suffix in SUFFIXES
    }
    if not counts:
        raise ValueError(f"scanned {len(ROOTS)} directories under {repository}, found no sources")
    return counts


def selftest() -> int:
    assert not violations({"boundary.cpp": MAX_LINES})
    result = violations({"grown.cpp": MAX_LINES + 1})
    assert result == ["grown.cpp: 1001 lines exceeds the 1000-line limit"]
    assert source_counts(REPOSITORY), "the production scanner must find real source files"
    try:
        source_counts(REPOSITORY / "CMakeLists.txt")
    except ValueError as error:
        assert "missing source directories" in str(error)
    else:
        raise AssertionError("missing source corpus must refuse, never pass with zero files")
    print("source structure self-test passed")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:]:
        print("usage: tools/check_source_structure.py [--selftest]", file=sys.stderr)
        return 2

    try:
        counts = source_counts(REPOSITORY)
    except ValueError as error:
        print(f"source structure check refused: {error}", file=sys.stderr)
        return 2
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
