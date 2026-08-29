from __future__ import annotations

import contextlib
import hashlib
import io
import json
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


class ImportFoundationTest(unittest.TestCase):
    def test_archive_rejects_traversal_duplicate_and_symlink_entries(self) -> None:
        cases: list[tuple[str, callable]] = [
            ("../escape", lambda archive: archive.writestr("../escape", b"bad")),
            (
                "duplicate",
                lambda archive: (
                    archive.writestr("base/value", b"one"),
                    archive.writestr("base/value", b"two"),
                ),
            ),
        ]
        for label, populate in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                jar = Path(directory) / "fixture.jar"
                with zipfile.ZipFile(jar, "w") as archive:
                    populate(archive)
                with zipfile.ZipFile(jar) as archive:
                    with self.assertRaises(rigel_assets.AssetImportError):
                        rigel_assets.indexed_archive(archive)

        with tempfile.TemporaryDirectory() as directory:
            jar = Path(directory) / "fixture.jar"
            info = zipfile.ZipInfo("base/link")
            info.create_system = 3
            info.external_attr = (0o120777 << 16)
            with zipfile.ZipFile(jar, "w") as archive:
                archive.writestr(info, b"target")
            with zipfile.ZipFile(jar) as archive:
                with self.assertRaisesRegex(rigel_assets.AssetImportError, "symbolic-link"):
                    rigel_assets.indexed_archive(archive)

    def test_tree_hash_is_path_and_content_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first = Path(first_dir)
            second = Path(second_dir)
            rigel_assets.write_output(first, "z/item.bin", b"last")
            rigel_assets.write_output(first, "a/item.bin", b"first")
            rigel_assets.write_output(second, "a/item.bin", b"first")
            rigel_assets.write_output(second, "z/item.bin", b"last")
            self.assertEqual(rigel_assets.sha256_tree(first), rigel_assets.sha256_tree(second))
            (second / "z/item.bin").write_bytes(b"changed")
            self.assertNotEqual(rigel_assets.sha256_tree(first), rigel_assets.sha256_tree(second))

    def test_publish_replaces_stale_tree_and_writes_deterministic_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            rigel_assets.write_output(old, "stale.txt", b"stale")
            staging = root / ".rigel/staging"
            rigel_assets.write_output(staging, "fresh.txt", b"fresh")
            provenance = {
                "schema": 1,
                "jar_sha256": hashlib.sha256(b"jar").hexdigest(),
                "output_tree_sha256": rigel_assets.sha256_tree(staging),
            }

            rigel_assets.publish_generated_tree(root, staging, provenance)

            self.assertFalse((old / "stale.txt").exists())
            self.assertEqual((old / "fresh.txt").read_bytes(), b"fresh")
            persisted = json.loads(
                (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_text(encoding="utf-8")
            )
            self.assertEqual(persisted, provenance)
            self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))

    def test_output_path_rejects_escape(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(rigel_assets.AssetImportError):
                rigel_assets.write_output(Path(directory), "../escape", b"bad")


if __name__ == "__main__":
    unittest.main()
