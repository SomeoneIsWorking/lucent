"""Contract tests for the shipping C++ quality-tool input selection."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import check_cpp_quality


class CppQualityInputTests(unittest.TestCase):
    def test_relative_build_directory_is_root_relative(self) -> None:
        self.assertEqual(
            check_cpp_quality.resolve_build_dir("build/ci"),
            (ROOT / "build/ci").resolve(),
        )

    def test_configured_first_party_sources_are_deduplicated_and_filtered(self) -> None:
        build_root = ROOT / "build"
        build_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build_root) as temporary:
            database = Path(temporary) / "compile_commands.json"
            config = (ROOT / "src/config.cpp").resolve()
            zip_source = (ROOT / "src/zip.cpp").resolve()
            database.write_text(
                json.dumps(
                    [
                        {"directory": str(ROOT), "file": "src/config.cpp"},
                        {"directory": str(ROOT), "file": str(config)},
                        {"directory": str(ROOT), "file": "vendor/not_first_party.cpp"},
                    ]
                ),
                encoding="utf-8",
            )
            selected = check_cpp_quality.configured_translation_units(
                database, {config, zip_source}
            )
        self.assertEqual(selected, [str(config)])

    def test_missing_malformed_and_empty_databases_refuse(self) -> None:
        build_root = ROOT / "build"
        build_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build_root) as temporary:
            database = Path(temporary) / "compile_commands.json"
            with self.assertRaisesRegex(SystemExit, "is missing"):
                check_cpp_quality.configured_translation_units(database, set())

            database.write_text("not json", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "cannot read"):
                check_cpp_quality.configured_translation_units(database, set())

            database.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "scanned 0 entries"):
                check_cpp_quality.configured_translation_units(database, set())


if __name__ == "__main__":
    unittest.main()
