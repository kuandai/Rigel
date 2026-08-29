from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile


sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import rigel_assets


def write_jar(path: Path, entries: dict[str, bytes] | None = None) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in (entries or {"base/version.txt": b"test"}).items():
            archive.writestr(name, data)


class ProvisioningTest(unittest.TestCase):
    def test_stage_copies_valid_jar_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "external.jar"
            write_jar(source)

            staged = rigel_assets.stage_jar(root, source)

            self.assertEqual(staged, root / ".rigel/source/Cosmic-Reach.jar")
            self.assertEqual(staged.read_bytes(), source.read_bytes())
            self.assertFalse(any(staged.parent.glob(".*.tmp")))

    def test_resolution_priority_is_explicit_environment_staged(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staged = root / ".rigel/source/Cosmic-Reach.jar"
            environment = root / "environment.jar"
            explicit = root / "explicit.jar"
            staged.parent.mkdir(parents=True)
            for path in (staged, environment, explicit):
                write_jar(path, {"identity": path.name.encode()})

            previous = os.environ.get(rigel_assets.JAR_ENVIRONMENT_VARIABLE)
            os.environ[rigel_assets.JAR_ENVIRONMENT_VARIABLE] = str(environment)
            try:
                self.assertEqual(rigel_assets.resolve_jar(root)[0], environment)
                self.assertEqual(rigel_assets.resolve_jar(root, explicit)[0], explicit)
            finally:
                if previous is None:
                    os.environ.pop(rigel_assets.JAR_ENVIRONMENT_VARIABLE, None)
                else:
                    os.environ[rigel_assets.JAR_ENVIRONMENT_VARIABLE] = previous

            self.assertEqual(rigel_assets.resolve_jar(root)[0], staged)

    def test_missing_jar_diagnostic_is_actionable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            previous = os.environ.pop(rigel_assets.JAR_ENVIRONMENT_VARIABLE, None)
            try:
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = rigel_assets.status(Path(directory))
            finally:
                if previous is not None:
                    os.environ[rigel_assets.JAR_ENVIRONMENT_VARIABLE] = previous

            self.assertEqual(result, 1)
            self.assertIn("rigel_assets.py stage", output.getvalue())
            self.assertIn("Source-only builds", output.getvalue())

    def test_stage_rejects_non_zip_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "not-a-jar"
            source.write_text("not a zip", encoding="utf-8")
            with self.assertRaisesRegex(rigel_assets.AssetImportError, "not a readable"):
                rigel_assets.stage_jar(root, source)


if __name__ == "__main__":
    unittest.main()
