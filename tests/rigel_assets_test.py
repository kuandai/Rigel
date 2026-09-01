from __future__ import annotations

import contextlib
import hashlib
import io
import json
import multiprocessing
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import zlib
import zipfile


sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import rigel_assets
from tests import single_cuboid_fixture


def write_jar(path: Path, entries: dict[str, bytes] | None = None) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in (entries or {"base/version.txt": b"test"}).items():
            archive.writestr(name, data)


def encoded_json(value: object) -> bytes:
    return json.dumps(value).encode("utf-8")


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def synthetic_png(
    width: int = 16,
    height: int = 16,
    transparent_pixels: set[tuple[int, int]] | None = None,
    fractional_pixels: dict[tuple[int, int], int] | None = None,
    interlace: int = 0,
) -> bytes:
    transparent_pixels = transparent_pixels or set()
    fractional_pixels = fractional_pixels or {}
    pixels = b"".join(
        b"\0" + b"".join(
            b"\xff\xff\xff" + (
                bytes((fractional_pixels[(x, y)],))
                if (x, y) in fractional_pixels
                else b"\0" if (x, y) in transparent_pixels else b"\xff"
            )
            for x in range(width)
        )
        for y in range(height)
    )
    return (
        rigel_assets.PNG_SIGNATURE
        + png_chunk(
            b"IHDR",
            struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, interlace),
        )
        + png_chunk(b"IDAT", zlib.compress(pixels))
        + png_chunk(b"IEND", b"")
    )


def png_with_lowercase_reserved_chunk() -> bytes:
    payload = synthetic_png()
    header_end = len(rigel_assets.PNG_SIGNATURE) + 12 + 13
    return (
        payload[:header_end]
        + png_chunk(b"tesT", b"")
        + payload[header_end:]
    )


def truecolor_png_with_transparency_before_palette() -> bytes:
    return (
        rigel_assets.PNG_SIGNATURE
        + png_chunk(
            b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
        )
        + png_chunk(b"tRNS", struct.pack(">HHH", 0, 0, 0))
        + png_chunk(b"PLTE", b"\xff\xff\xff")
        + png_chunk(b"IDAT", zlib.compress(b"\0\xff\xff\xff"))
        + png_chunk(b"IEND", b"")
    )


def synthetic_block_entries() -> dict[str, bytes]:
    return {
        "base/models/blocks/cube.json": encoded_json(
            {
                "textures": {
                    "all": {"fileName": "base:textures/blocks/test_stone.png"}
                },
                "cuboids": [
                    {
                        "localBounds": [0, 0, 0, 16, 16, 16],
                        "faces": {
                            name: {
                                "uv": [0, 0, 16, 16],
                                "ambientocclusion": True,
                                "cullFace": True,
                                "texture": "all",
                            }
                            for name in rigel_assets.FACE_VECTORS
                        },
                    }
                ],
            }
        ),
        "base/block_state_generators/variants.json": encoded_json(
            {
                "generators": [
                    {
                        "stringId": "base:test_variants",
                        "include": ["base:test_variant_full"],
                    },
                    {
                        "stringId": "base:test_variant_full",
                        "params": {"shape": "full"},
                        "modelName": "base:models/blocks/cube.json",
                        "overrides": {"isOpaque": False, "lightAttenuation": 2},
                    },
                ]
            }
        ),
        "base/blocks/test_stone.json": encoded_json(
            {
                "stringId": "test:stone",
                "defaultProperties": {
                    "modelName": "base:models/blocks/cube.json",
                    "stateGenerators": ["base:test_variants"],
                },
                "blockStates": {
                    "default": {},
                    "kind=bright": {
                        "lightLevelRed": 7,
                        "walkThrough": True,
                    },
                },
            }
        ),
        "base/textures/blocks/test_stone.png": synthetic_png(),
    }


def synthetic_cuboid_entries() -> dict[str, bytes]:
    return {
        "base/models/blocks/post.json": encoded_json(
            {
                "textures": {
                    "surface": {
                        "fileName": "base:textures/blocks/default_surface.png"
                    },
                    "accent": {
                        "fileName": "base:textures/blocks/default_accent.png"
                    },
                },
                "isTransparent": True,
                "cullsSelf": True,
                "cuboids": [
                    {
                        "localBounds": [-1, 0, 4, 17, 8, 12],
                        "inflate": 0.5,
                        "faces": {
                            "localPosX": {
                                "uv": [12, 2, 4, 14],
                                "texture": "surface",
                                "uvRotation": 270,
                                "ambientocclusion": True,
                                "cullFace": False,
                                "shadingFace": "localPosY",
                            }
                        },
                    },
                    {
                        "localBounds": [4, 8, 4, 12, 16, 12],
                        "faces": {
                            "localPosY": {
                                "uv": [1, 15, 15, 1],
                                "texture": "accent",
                                "uvRotation": 90,
                                "ambientocclusion": False,
                                "cullFace": True,
                            },
                            "localNegY": {
                                "uv": [15, 0, 16, 0],
                                "texture": "surface",
                                "ambientocclusion": False,
                                "cullFace": False,
                            }
                        },
                    },
                ],
            }
        ),
        "base/models/blocks/red_post.json": encoded_json(
            {
                "parent": "base:models/blocks/post.json",
                "textures": {
                    "surface": {
                        "fileName": "base:textures/blocks/red_surface.png"
                    },
                    "accent": {
                        "fileName": "base:textures/blocks/red_accent.png"
                    },
                },
            }
        ),
        "base/models/blocks/blue_post.json": encoded_json(
            {
                "parent": "base:models/blocks/red_post.json",
                "textures": {
                    "surface": {
                        "fileName": "base:textures/blocks/blue_surface.png"
                    },
                    "accent": {
                        "fileName": "base:textures/blocks/blue_accent.png"
                    },
                },
            }
        ),
        "base/block_state_generators/posts.json": encoded_json(
            {
                "generators": [
                    {
                        "stringId": "base:test_post_variant",
                        "params": {"shape": "generated"},
                        "modelName": "base:models/blocks/post.json",
                        "overrides": {"rotation": [0, 90, 0]},
                    }
                ]
            }
        ),
        "base/blocks/test_post.json": encoded_json(
            {
                "stringId": "test:post",
                "defaultProperties": {
                    "modelName": "base:models/blocks/red_post.json"
                },
                "blockStates": {
                    "default": {"stateGenerators": ["base:test_post_variant"]},
                    "color=blue": {
                        "modelName": "base:models/blocks/blue_post.json"
                    },
                },
            }
        ),
        **{
            f"base/textures/blocks/{name}.png": synthetic_png()
            for name in (
                "default_surface", "default_accent", "red_surface",
                "red_accent", "blue_surface", "blue_accent",
            )
        },
    }


def compile_cuboid_fixture(root: Path) -> Path:
    jar = root / "fixture.jar"
    write_jar(jar, synthetic_cuboid_entries())
    output = root / "output"
    with zipfile.ZipFile(jar) as archive:
        indexed = rigel_assets.indexed_archive(archive)
        rigel_assets.extract_direct_assets(archive, indexed, output)
        rigel_assets.compile_blocks(archive, indexed, output)
    return output


class TextureAlphaClassificationTest(unittest.TestCase):
    def test_classifies_opaque_binary_and_fractional_rgba(self) -> None:
        cases = (
            (synthetic_png(), rigel_assets.TextureAlphaClass.FULLY_OPAQUE),
            (
                synthetic_png(transparent_pixels={(0, 0), (8, 8)}),
                rigel_assets.TextureAlphaClass.BINARY,
            ),
            (
                synthetic_png(fractional_pixels={(0, 0): 1, (8, 8): 128}),
                rigel_assets.TextureAlphaClass.FRACTIONAL,
            ),
        )
        for payload, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(
                    rigel_assets.classify_png_alpha(payload, "fixture.png"),
                    expected,
                )

    def test_malformed_and_unsupported_pngs_fail_closed(self) -> None:
        cases = (
            (synthetic_png()[:-1], "incomplete"),
            (synthetic_png(interlace=1), "unsupported"),
            (png_with_lowercase_reserved_chunk(), "lowercase reserved"),
            (
                truecolor_png_with_transparency_before_palette(),
                "malformed PNG palette",
            ),
        )
        for payload, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(
                rigel_assets.AssetImportError, diagnostic
            ):
                rigel_assets.classify_png_alpha(payload, "fixture.png")


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
                os.environ.pop(rigel_assets.JAR_ENVIRONMENT_VARIABLE)
                self.assertEqual(rigel_assets.resolve_jar(root)[0], staged)
            finally:
                if previous is None:
                    os.environ.pop(rigel_assets.JAR_ENVIRONMENT_VARIABLE, None)
                else:
                    os.environ[rigel_assets.JAR_ENVIRONMENT_VARIABLE] = previous

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

    def test_status_rejects_tampered_generated_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            rigel_assets.synchronize(root, jar, required_identifiers=("test:stone",))
            (root / ".rigel/assets/blocks/test__stone.yaml").write_text(
                "tampered", encoding="utf-8"
            )

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = rigel_assets.status(root, jar)

            self.assertEqual(result, 2)
            self.assertIn("Generated assets: not synchronized", output.getvalue())


class ImportFoundationTest(unittest.TestCase):
    def _prepare_publication(self, root: Path) -> tuple[Path, dict[str, object]]:
        assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
        rigel_assets.write_output(assets, "old.txt", b"old")
        rigel_assets.atomic_write_json(
            root / rigel_assets.PROVENANCE_RELATIVE_PATH,
            {
                "generation": "old",
                "jar_sha256": hashlib.sha256(b"old-source").hexdigest(),
                "output_tree_sha256": rigel_assets.sha256_tree(assets),
            },
        )
        staging = root / ".rigel/.assets-staging-fixture"
        rigel_assets.write_output(staging, "new.txt", b"new")
        return staging, {
            "generation": "new",
            "jar_sha256": hashlib.sha256(b"new-source").hexdigest(),
            "output_tree_sha256": rigel_assets.sha256_tree(staging),
        }

    def _assert_publication(self, root: Path, generation: str) -> None:
        assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
        provenance = json.loads(
            (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(provenance["generation"], generation)
        self.assertEqual(
            provenance["output_tree_sha256"], rigel_assets.sha256_tree(assets)
        )
        self.assertEqual(
            (assets / f"{generation}.txt").read_bytes(), generation.encode()
        )
        other = "new" if generation == "old" else "old"
        self.assertFalse((assets / f"{other}.txt").exists())
        self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))
        self.assertFalse(any((root / ".rigel").glob(".assets-staging-*")))
        self.assertFalse(any((root / ".rigel").glob(".assets.failed")))

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

    def test_publish_recovers_when_moving_previous_tree_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            real_replace = os.replace

            def fail_move_previous(source: object, destination: object) -> None:
                if Path(source) == assets:
                    raise OSError("injected previous-tree move failure")
                real_replace(source, destination)

            with mock.patch.object(
                rigel_assets.os, "replace", side_effect=fail_move_previous
            ):
                with self.assertRaisesRegex(OSError, "previous-tree"):
                    rigel_assets.publish_generated_tree(root, staging, provenance)

            self._assert_publication(root, "old")

    def test_publish_recovers_when_installing_staging_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            real_replace = os.replace

            def fail_install(source: object, destination: object) -> None:
                if Path(source) == staging and Path(destination) == assets:
                    raise OSError("injected staging install failure")
                real_replace(source, destination)

            with mock.patch.object(rigel_assets.os, "replace", side_effect=fail_install):
                with self.assertRaisesRegex(OSError, "staging install"):
                    rigel_assets.publish_generated_tree(root, staging, provenance)

            self._assert_publication(root, "old")

    def test_failed_first_install_leaves_no_public_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging = root / ".rigel/.assets-staging-fixture"
            rigel_assets.write_output(staging, "new.txt", b"new")
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            real_replace = os.replace

            def fail_install(source: object, destination: object) -> None:
                if Path(source) == staging and Path(destination) == assets:
                    raise OSError("injected first staging install failure")
                real_replace(source, destination)

            with mock.patch.object(
                rigel_assets.os, "replace", side_effect=fail_install
            ):
                with self.assertRaisesRegex(OSError, "first staging install"):
                    rigel_assets.publish_generated_tree(
                        root, staging, {"generation": "new"}
                    )

            self.assertFalse(assets.exists())
            self.assertFalse(provenance_path.exists())
            self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))
            self.assertFalse(any((root / ".rigel").glob(".assets-staging-*")))

    def test_publish_recovers_when_provenance_write_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            real_atomic_write = rigel_assets.atomic_write_json

            def fail_provenance_write(
                destination: Path, value: dict[str, object]
            ) -> None:
                if destination == provenance_path:
                    raise OSError("injected provenance write failure")
                real_atomic_write(destination, value)

            with mock.patch.object(
                rigel_assets,
                "atomic_write_json",
                side_effect=fail_provenance_write,
            ):
                with self.assertRaisesRegex(OSError, "provenance write"):
                    rigel_assets.publish_generated_tree(root, staging, provenance)

            self._assert_publication(root, "old")

    def test_failed_first_provenance_write_leaves_no_public_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging = root / ".rigel/.assets-staging-fixture"
            rigel_assets.write_output(staging, "new.txt", b"new")
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            real_atomic_write = rigel_assets.atomic_write_json

            def fail_provenance_write(
                destination: Path, value: dict[str, object]
            ) -> None:
                if destination == provenance_path:
                    raise OSError("injected first provenance write failure")
                real_atomic_write(destination, value)

            with mock.patch.object(
                rigel_assets,
                "atomic_write_json",
                side_effect=fail_provenance_write,
            ):
                with self.assertRaisesRegex(OSError, "first provenance write"):
                    rigel_assets.publish_generated_tree(
                        root, staging, {"generation": "new"}
                    )

            self.assertFalse(assets.exists())
            self.assertFalse(provenance_path.exists())
            self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))
            self.assertFalse(any((root / ".rigel").glob(".assets-staging-*")))

    def test_publish_restores_provenance_with_atomic_rename(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            real_atomic_write = rigel_assets.atomic_write_json
            real_atomic_copy = rigel_assets.atomic_copy
            restoration_attempts = 0

            def publish_then_fail(destination: Path, value: dict[str, object]) -> None:
                real_atomic_write(destination, value)
                if destination == provenance_path:
                    raise OSError("injected post-publication failure")

            def fail_first_restore(source: Path, destination: Path) -> None:
                nonlocal restoration_attempts
                if (
                    destination == provenance_path
                    and source.parent.name.startswith(".assets-previous-")
                ):
                    restoration_attempts += 1
                    if restoration_attempts == 1:
                        raise OSError("injected provenance restore failure")
                real_atomic_copy(source, destination)

            with mock.patch.object(
                rigel_assets, "atomic_write_json", side_effect=publish_then_fail
            ), mock.patch.object(
                rigel_assets, "atomic_copy", side_effect=fail_first_restore
            ):
                with self.assertRaisesRegex(OSError, "post-publication"):
                    rigel_assets.publish_generated_tree(root, staging, provenance)

            self.assertGreaterEqual(restoration_attempts, 2)
            self._assert_publication(root, "old")

    def test_publish_retries_rollback_directory_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            real_rmtree = rigel_assets.shutil.rmtree
            real_atomic_write = rigel_assets.atomic_write_json
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            cleanup_attempts = 0

            def fail_provenance_write(
                destination: Path, value: dict[str, object]
            ) -> None:
                if destination == provenance_path:
                    raise OSError("injected provenance write failure")
                real_atomic_write(destination, value)

            def fail_first_cleanup(path: object, *args: object, **kwargs: object) -> None:
                nonlocal cleanup_attempts
                if Path(path).name.startswith(".assets-previous-"):
                    cleanup_attempts += 1
                    if cleanup_attempts == 1:
                        raise OSError("injected rollback cleanup failure")
                real_rmtree(path, *args, **kwargs)

            with mock.patch.object(
                rigel_assets,
                "atomic_write_json",
                side_effect=fail_provenance_write,
            ), mock.patch.object(
                rigel_assets.shutil, "rmtree", side_effect=fail_first_cleanup
            ):
                with self.assertRaisesRegex(OSError, "provenance write"):
                    rigel_assets.publish_generated_tree(root, staging, provenance)

            self.assertGreaterEqual(cleanup_attempts, 2)
            self._assert_publication(root, "old")

    def test_publish_retries_committed_backup_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            real_rmtree = rigel_assets.shutil.rmtree
            cleanup_attempts = 0

            def fail_first_cleanup(path: object, *args: object, **kwargs: object) -> None:
                nonlocal cleanup_attempts
                if Path(path).name.startswith(".assets-previous-"):
                    cleanup_attempts += 1
                    if cleanup_attempts == 1:
                        raise OSError("injected backup cleanup failure")
                real_rmtree(path, *args, **kwargs)

            with mock.patch.object(
                rigel_assets.shutil, "rmtree", side_effect=fail_first_cleanup
            ):
                rigel_assets.publish_generated_tree(root, staging, provenance)

            self.assertGreaterEqual(cleanup_attempts, 2)
            self._assert_publication(root, "new")

    def test_publish_reaps_committed_backup_after_persistent_cleanup_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging, provenance = self._prepare_publication(root)
            provenance["output_tree_sha256"] = rigel_assets.sha256_tree(staging)
            real_rmtree = rigel_assets.shutil.rmtree

            def fail_cleanup(path: object, *args: object, **kwargs: object) -> None:
                if Path(path).name.startswith(".assets-previous-"):
                    raise OSError("injected persistent backup cleanup failure")
                real_rmtree(path, *args, **kwargs)

            with mock.patch.object(
                rigel_assets.shutil, "rmtree", side_effect=fail_cleanup
            ):
                rigel_assets.publish_generated_tree(root, staging, provenance)

            backups = list((root / ".rigel").glob(".assets-previous-*"))
            self.assertEqual(len(backups), 1)

            next_staging = root / ".rigel/.assets-staging-next"
            rigel_assets.write_output(next_staging, "latest.txt", b"latest")
            next_provenance = {
                "generation": "latest",
                "output_tree_sha256": rigel_assets.sha256_tree(next_staging),
            }
            with mock.patch.object(
                rigel_assets.shutil, "rmtree", side_effect=fail_cleanup
            ):
                with self.assertRaisesRegex(
                    OSError, "persistent backup cleanup"
                ):
                    rigel_assets.publish_generated_tree(
                        root, next_staging, next_provenance
                    )

            self.assertEqual(
                len(list((root / ".rigel").glob(".assets-previous-*"))), 1
            )
            rigel_assets.publish_generated_tree(
                root, next_staging, next_provenance
            )

            self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))
            self.assertEqual(
                (root / ".rigel/assets/latest.txt").read_bytes(), b"latest"
            )
            self.assertEqual(
                json.loads(
                    (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_text(
                        encoding="utf-8"
                    )
                ),
                next_provenance,
            )

    def test_concurrent_publications_keep_tree_and_provenance_together(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_staging = root / ".rigel/.assets-staging-first"
            second_staging = root / ".rigel/.assets-staging-second"
            rigel_assets.write_output(first_staging, "first.txt", b"first")
            rigel_assets.write_output(second_staging, "second.txt", b"second")
            first_provenance = {
                "generation": "first",
                "output_tree_sha256": rigel_assets.sha256_tree(first_staging),
            }
            second_provenance = {
                "generation": "second",
                "output_tree_sha256": rigel_assets.sha256_tree(second_staging),
            }
            first_at_provenance = threading.Event()
            second_lock_attempted = threading.Event()
            errors: list[BaseException] = []
            real_lock = rigel_assets._publication_lock
            real_atomic_write = rigel_assets.atomic_write_json

            @contextlib.contextmanager
            def observed_lock(lock_root: Path):
                if threading.current_thread().name == "second-publisher":
                    second_lock_attempted.set()
                with real_lock(lock_root):
                    yield

            def coordinated_write(
                destination: Path, value: dict[str, object]
            ) -> None:
                if value.get("generation") == "first":
                    first_at_provenance.set()
                    if not second_lock_attempted.wait(timeout=5):
                        raise AssertionError(
                            "second publisher did not contend for publication"
                        )
                real_atomic_write(destination, value)

            def publish(
                staging: Path, provenance: dict[str, object]
            ) -> None:
                try:
                    rigel_assets.publish_generated_tree(root, staging, provenance)
                except BaseException as error:
                    errors.append(error)

            with mock.patch.object(
                rigel_assets, "_publication_lock", side_effect=observed_lock
            ), mock.patch.object(
                rigel_assets, "atomic_write_json", side_effect=coordinated_write
            ):
                first = threading.Thread(
                    target=publish,
                    args=(first_staging, first_provenance),
                    name="first-publisher",
                )
                second = threading.Thread(
                    target=publish,
                    args=(second_staging, second_provenance),
                    name="second-publisher",
                )
                first.start()
                self.assertTrue(first_at_provenance.wait(timeout=5))
                second.start()
                first.join(timeout=5)
                second.join(timeout=5)

            self.assertFalse(first.is_alive())
            self.assertFalse(second.is_alive())
            self.assertEqual(errors, [])
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            published = json.loads(
                (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                published["output_tree_sha256"],
                rigel_assets.sha256_tree(assets),
            )
            self.assertEqual(published["generation"], "second")
            self.assertTrue((assets / "second.txt").is_file())

    def test_interrupted_publication_is_recovered_before_the_next_read(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_jar = root / "first.jar"
            second_jar = root / "second.jar"
            first_entries = synthetic_block_entries()
            first_entries["base/version.txt"] = b"first"
            second_entries = synthetic_block_entries()
            second_entries["base/version.txt"] = b"second"
            second_entries["base/sounds/new.ogg"] = b"new"
            write_jar(first_jar, first_entries)
            write_jar(second_jar, second_entries)
            previous, _ = rigel_assets.synchronize(
                root, first_jar, required_identifiers=("test:stone",)
            )

            def interrupt_before_provenance() -> None:
                real_atomic_write = rigel_assets.atomic_write_json

                def exit_process(
                    destination: Path, value: dict[str, object]
                ) -> None:
                    if destination == root / rigel_assets.PROVENANCE_RELATIVE_PATH:
                        os._exit(73)
                    real_atomic_write(destination, value)

                with mock.patch.object(
                    rigel_assets, "atomic_write_json", side_effect=exit_process
                ):
                    rigel_assets.synchronize(
                        root,
                        second_jar,
                        force=True,
                        required_identifiers=("test:stone",),
                    )

            process = multiprocessing.get_context("fork").Process(
                target=interrupt_before_provenance
            )
            process.start()
            process.join(timeout=10)

            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode, 73)
            self.assertTrue(
                rigel_assets.current_import_matches(
                    root, str(previous["jar_sha256"])
                )
            )
            recovered = rigel_assets.read_provenance(root)
            self.assertIsNotNone(recovered)
            assert recovered is not None
            self.assertEqual(
                recovered["output_tree_sha256"], previous["output_tree_sha256"]
            )
            self.assertEqual(recovered.get("source_version"), "first")
            self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))

            updated, changed = rigel_assets.synchronize(
                root, second_jar, required_identifiers=("test:stone",)
            )

            self.assertTrue(changed)
            self.assertEqual(updated.get("source_version"), "second")
            self.assertTrue(
                rigel_assets.current_import_matches(
                    root, str(updated["jar_sha256"])
                )
            )

    def test_interrupted_staging_is_reclaimed_before_the_next_operation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            previous, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )

            for _ in range(3):
                staging_populated = multiprocessing.Event()

                def stop_before_publication() -> None:
                    real_validate = rigel_assets.validate_generated_tree

                    def wait_with_populated_staging(
                        staging: Path,
                        required_identifiers: tuple[str, ...],
                    ) -> dict[str, int]:
                        if staging.name.startswith(rigel_assets.STAGING_PREFIX):
                            staging_populated.set()
                            threading.Event().wait()
                        return real_validate(staging, required_identifiers)

                    with mock.patch.object(
                        rigel_assets,
                        "validate_generated_tree",
                        side_effect=wait_with_populated_staging,
                    ):
                        rigel_assets.synchronize(
                            root,
                            jar,
                            force=True,
                            required_identifiers=("test:stone",),
                        )

                process = multiprocessing.get_context("fork").Process(
                    target=stop_before_publication
                )
                process.start()
                self.assertTrue(staging_populated.wait(timeout=10))
                observed = rigel_assets.read_provenance(root)
                self.assertIsNotNone(observed)
                assert observed is not None
                self.assertEqual(
                    observed["output_tree_sha256"],
                    previous["output_tree_sha256"],
                )
                self.assertEqual(
                    len(
                        list(
                            (root / ".rigel").glob(
                                f"{rigel_assets.STAGING_PREFIX}*"
                            )
                        )
                    ),
                    2,
                )
                process.kill()
                process.join(timeout=10)

                self.assertFalse(process.is_alive())
                self.assertLess(process.exitcode or 0, 0)
                self.assertEqual(
                    len(
                        list(
                            (root / ".rigel").glob(
                                f"{rigel_assets.STAGING_PREFIX}*"
                            )
                        )
                    ),
                    2,
                )

            recovered = rigel_assets.read_provenance(root)

            self.assertIsNotNone(recovered)
            assert recovered is not None
            self.assertEqual(
                recovered["output_tree_sha256"], previous["output_tree_sha256"]
            )
            self.assertEqual(
                rigel_assets.sha256_tree(
                    root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
                ),
                previous["output_tree_sha256"],
            )
            self.assertFalse(
                any(
                    (root / ".rigel").glob(f"{rigel_assets.STAGING_PREFIX}*")
                )
            )

    def test_staging_lease_is_locked_before_reapers_can_observe_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lease_created = threading.Event()
            continue_creation = threading.Event()
            staging_active = threading.Event()
            finish_staging = threading.Event()
            reaper_lock_attempted = threading.Event()
            first_reaper_finished = threading.Event()
            errors: list[BaseException] = []
            real_mkstemp = rigel_assets.tempfile.mkstemp
            real_publication_lock = rigel_assets._publication_lock

            def pause_after_lease_creation(*args: object, **kwargs: object):
                result = real_mkstemp(*args, **kwargs)
                if kwargs.get("prefix") == rigel_assets.STAGING_PREFIX:
                    lease_created.set()
                    if not continue_creation.wait(timeout=5):
                        raise AssertionError("staging creation was not resumed")
                return result

            @contextlib.contextmanager
            def observed_publication_lock(lock_root: Path):
                if threading.current_thread().name == "staging-reaper":
                    reaper_lock_attempted.set()
                with real_publication_lock(lock_root):
                    yield

            def create_staging() -> None:
                try:
                    with rigel_assets._generated_tree_staging(root) as staging:
                        staging_active.set()
                        if not finish_staging.wait(timeout=5):
                            raise AssertionError("active staging was not released")
                        self.assertTrue(staging.is_dir())
                except BaseException as error:
                    errors.append(error)

            def reap_staging() -> None:
                try:
                    with rigel_assets._publication_guard(root):
                        pass
                except BaseException as error:
                    errors.append(error)
                finally:
                    first_reaper_finished.set()

            with mock.patch.object(
                rigel_assets.tempfile,
                "mkstemp",
                side_effect=pause_after_lease_creation,
            ), mock.patch.object(
                rigel_assets,
                "_publication_lock",
                side_effect=observed_publication_lock,
            ):
                creator = threading.Thread(
                    target=create_staging, name="staging-creator"
                )
                reaper = threading.Thread(
                    target=reap_staging, name="staging-reaper"
                )
                creator.start()
                self.assertTrue(lease_created.wait(timeout=5))
                leases = list(
                    (root / ".rigel").glob(
                        f"{rigel_assets.STAGING_PREFIX}*"
                        f"{rigel_assets.STAGING_LEASE_SUFFIX}"
                    )
                )
                self.assertEqual(len(leases), 1)
                reaper.start()
                self.assertTrue(reaper_lock_attempted.wait(timeout=5))
                self.assertFalse(first_reaper_finished.is_set())

                continue_creation.set()
                self.assertTrue(staging_active.wait(timeout=5))
                reaper.join(timeout=5)
                self.assertFalse(reaper.is_alive())
                self.assertTrue(first_reaper_finished.is_set())

                with rigel_assets._publication_guard(root):
                    pass
                active_staging = leases[0].with_name(
                    leases[0].name[: -len(rigel_assets.STAGING_LEASE_SUFFIX)]
                )
                self.assertTrue(active_staging.is_dir())
                finish_staging.set()
                creator.join(timeout=5)

            self.assertFalse(creator.is_alive())
            self.assertEqual(errors, [])
            self.assertFalse(any((root / ".rigel").glob(".assets-staging-*")))

    def test_reader_waits_for_tree_and_provenance_publication(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            previous, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            staging = root / ".rigel/.assets-staging-reader"
            rigel_assets.shutil.copytree(assets, staging)
            rigel_assets.write_output(staging, "sounds/new.ogg", b"new")
            next_provenance = dict(previous)
            next_provenance["source_version"] = "next"
            next_provenance["output_tree_sha256"] = rigel_assets.sha256_tree(staging)
            next_provenance["counts"] = rigel_assets.validate_generated_tree(
                staging, required_identifiers=("test:stone",)
            )
            publisher_paused = threading.Event()
            continue_publication = threading.Event()
            reader_lock_attempted = threading.Event()
            reader_finished = threading.Event()
            observed: list[bool] = []
            errors: list[BaseException] = []
            real_lock = rigel_assets._publication_lock
            real_atomic_write = rigel_assets.atomic_write_json

            @contextlib.contextmanager
            def observed_lock(lock_root: Path):
                if threading.current_thread().name == "asset-reader":
                    reader_lock_attempted.set()
                with real_lock(lock_root):
                    yield

            def pause_provenance_write(
                destination: Path, value: dict[str, object]
            ) -> None:
                if destination != root / rigel_assets.PROVENANCE_RELATIVE_PATH:
                    real_atomic_write(destination, value)
                    return
                publisher_paused.set()
                if not continue_publication.wait(timeout=5):
                    raise AssertionError("reader did not attempt the publication lock")
                real_atomic_write(destination, value)

            def publish() -> None:
                try:
                    rigel_assets.publish_generated_tree(
                        root, staging, next_provenance
                    )
                except BaseException as error:
                    errors.append(error)

            def read() -> None:
                try:
                    observed.append(
                        rigel_assets.current_import_matches(
                            root, str(next_provenance["jar_sha256"])
                        )
                    )
                except BaseException as error:
                    errors.append(error)
                finally:
                    reader_finished.set()

            with mock.patch.object(
                rigel_assets, "_publication_lock", side_effect=observed_lock
            ), mock.patch.object(
                rigel_assets,
                "atomic_write_json",
                side_effect=pause_provenance_write,
            ):
                publisher = threading.Thread(target=publish, name="asset-publisher")
                reader = threading.Thread(target=read, name="asset-reader")
                publisher.start()
                self.assertTrue(publisher_paused.wait(timeout=5))
                reader.start()
                self.assertTrue(reader_lock_attempted.wait(timeout=5))
                self.assertFalse(reader_finished.is_set())
                continue_publication.set()
                publisher.join(timeout=5)
                reader.join(timeout=5)

            self.assertFalse(publisher.is_alive())
            self.assertFalse(reader.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(observed, [True])
            self.assertEqual(
                rigel_assets.read_provenance(root).get("source_version"), "next"
            )
            self.assertTrue((assets / "sounds/new.ogg").is_file())

    def test_snapshot_rejects_generated_tree_outputs_without_mutation(self) -> None:
        for output_suffix in (Path(), Path("build-snapshots")):
            with self.subTest(output_suffix=output_suffix):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    self._prepare_publication(root)
                    assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
                    provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
                    provenance = rigel_assets.read_provenance(root)
                    self.assertIsNotNone(provenance)
                    assert provenance is not None
                    before_hash = rigel_assets.sha256_tree(assets)
                    before_provenance = provenance_path.read_bytes()
                    before_files = {
                        path.relative_to(assets): path.read_bytes()
                        for path in assets.rglob("*")
                        if path.is_file()
                    }
                    output = assets / output_suffix

                    error = io.StringIO()
                    with contextlib.redirect_stderr(error):
                        result = rigel_assets.main(
                            [
                                "--root",
                                str(root),
                                "snapshot",
                                "--output",
                                str(output),
                                "--jar-sha256",
                                str(provenance["jar_sha256"]),
                            ]
                        )

                    self.assertEqual(result, 1)
                    self.assertIn("must not overlap", error.getvalue())
                    self.assertEqual(rigel_assets.sha256_tree(assets), before_hash)
                    self.assertEqual(provenance_path.read_bytes(), before_provenance)
                    self.assertEqual(
                        {
                            path.relative_to(assets): path.read_bytes()
                            for path in assets.rglob("*")
                            if path.is_file()
                        },
                        before_files,
                    )
                    if output_suffix.parts:
                        self.assertFalse(output.exists())

    def test_pre_handoff_snapshots_have_a_fixed_single_generation_bound(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "asset-root"
            snapshots = Path(directory) / "snapshots"
            self._prepare_publication(root)

            for generation in range(6):
                if generation:
                    staging = root / f".rigel/.assets-staging-{generation}"
                    rigel_assets.write_output(
                        staging,
                        f"generation-{generation}.bin",
                        bytes([generation]) * (generation * 17),
                    )
                    provenance = {
                        "generation": generation,
                        "jar_sha256": hashlib.sha256(
                            f"source-{generation}".encode()
                        ).hexdigest(),
                        "output_tree_sha256": rigel_assets.sha256_tree(staging),
                    }
                    rigel_assets.publish_generated_tree(
                        root, staging, provenance
                    )
                else:
                    provenance = rigel_assets.read_provenance(root)
                    self.assertIsNotNone(provenance)
                    assert provenance is not None

                snapshot = rigel_assets.snapshot_generated_assets(
                    root, snapshots, str(provenance["jar_sha256"])
                )
                generations = [
                    path
                    for path in snapshots.iterdir()
                    if path.is_dir() and rigel_assets._is_sha256(path.name)
                ]
                self.assertEqual(generations, [snapshot])
                self.assertEqual(
                    sum(
                        path.stat().st_size
                        for path in snapshot.rglob("*")
                        if path.is_file()
                    ),
                    sum(
                        path.stat().st_size
                        for path in (
                            root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
                        ).rglob("*")
                        if path.is_file()
                    ),
                )
                self.assertFalse(
                    (
                        snapshots
                        / rigel_assets.SNAPSHOT_ACTIVE_GENERATION_FILENAME
                    ).exists()
                )

    def test_snapshot_copy_failure_preserves_the_assembly_generation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            test_root = Path(directory)
            root = test_root / "asset-root"
            snapshots = test_root / "snapshots"
            assembly = test_root / "embedded/resources.S"
            next_staging, next_provenance = self._prepare_publication(root)
            first_provenance = rigel_assets.read_provenance(root)
            self.assertIsNotNone(first_provenance)
            assert first_provenance is not None
            first = rigel_assets.snapshot_generated_assets(
                root, snapshots, str(first_provenance["jar_sha256"])
            )
            rigel_assets.retire_generated_asset_snapshots(snapshots, first)

            rigel_assets.write_output(next_staging, "new.txt", b"new")
            rigel_assets.publish_generated_tree(
                root, next_staging, next_provenance
            )
            assembly.parent.mkdir()
            assembly.write_text(
                f'.incbin "{first / "old.txt"}"\n', encoding="utf-8"
            )
            second = rigel_assets.snapshot_generated_assets(
                root,
                snapshots,
                str(next_provenance["jar_sha256"]),
                assembly,
            )
            assembly.write_text(
                f'.incbin "{second / "new.txt"}"\n', encoding="utf-8"
            )

            final_staging = root / ".rigel/.assets-staging-final"
            rigel_assets.write_output(final_staging, "final.txt", b"final")
            final_provenance = {
                "generation": "final",
                "jar_sha256": hashlib.sha256(b"final-source").hexdigest(),
                "output_tree_sha256": rigel_assets.sha256_tree(final_staging),
            }
            rigel_assets.publish_generated_tree(
                root, final_staging, final_provenance
            )
            with mock.patch.object(
                rigel_assets.shutil,
                "copytree",
                side_effect=OSError("injected snapshot copy failure"),
            ), self.assertRaisesRegex(OSError, "injected snapshot copy failure"):
                rigel_assets.snapshot_generated_assets(
                    root,
                    snapshots,
                    str(final_provenance["jar_sha256"]),
                    assembly,
                )

            self.assertFalse(first.exists())
            self.assertTrue(second.is_dir())
            self.assertEqual(
                sorted(
                    path.name
                    for path in snapshots.iterdir()
                    if path.is_dir()
                ),
                [second.name],
            )
            marker = json.loads(
                (
                    snapshots
                    / rigel_assets.SNAPSHOT_ACTIVE_GENERATION_FILENAME
                ).read_text(encoding="utf-8")
            )
            self.assertEqual(marker["tree_sha256"], second.name)

    def test_cmake_embeds_a_locked_immutable_generated_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            test_root = Path(directory)
            root = test_root / "asset-root"
            project = test_root / "project"
            build = test_root / "build"
            snapshots = build / "generated-resource-snapshots"
            project.mkdir()
            staging, provenance = self._prepare_publication(root)
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            publication_paused = multiprocessing.Event()
            continue_publication = multiprocessing.Event()

            def pause_with_public_tree_absent() -> None:
                real_replace = os.replace

                def pause_after_previous_tree_move(
                    source: object, destination: object
                ) -> None:
                    source_path = Path(source)
                    destination_path = Path(destination)
                    real_replace(source, destination)
                    if (
                        source_path == assets
                        and destination_path.name == "assets"
                        and destination_path.parent.name.startswith(
                            ".assets-previous-"
                        )
                    ):
                        publication_paused.set()
                        if not continue_publication.wait(timeout=15):
                            os._exit(74)

                with mock.patch.object(
                    rigel_assets.os,
                    "replace",
                    side_effect=pause_after_previous_tree_move,
                ):
                    rigel_assets.publish_generated_tree(
                        root, staging, provenance
                    )

            cmake = shutil.which("cmake")
            self.assertIsNotNone(cmake)
            assert cmake is not None
            build_environment = dict(os.environ)
            build_environment["CCACHE_DISABLE"] = "true"
            importer = Path(rigel_assets.__file__).resolve()
            asset_resources = importer.parent.parent / "cmake/AssetResources.cmake"
            (project / "dummy.cpp").write_text(
                "int generated_resource_dummy() { return 0; }\n",
                encoding="utf-8",
            )
            (project / "CMakeLists.txt").write_text(
                f"""cmake_minimum_required(VERSION 3.20)
project(GeneratedResourceSnapshot LANGUAGES CXX ASM)
include("{asset_resources}")
add_library(Dummy STATIC dummy.cpp)
rigel_snapshot_generated_resources(
    GENERATED_ROOT
    "{root}"
    "{snapshots}"
    "{sys.executable}"
    "{importer}"
    "{provenance['jar_sha256']}"
    "{build / 'embedded/Dummy_resources.S'}")
target_embed_resources(Dummy "${{GENERATED_ROOT}}")
""",
                encoding="utf-8",
            )

            publisher = multiprocessing.get_context("fork").Process(
                target=pause_with_public_tree_absent
            )
            configure: subprocess.Popen[str] | None = None
            try:
                publisher.start()
                self.assertTrue(publication_paused.wait(timeout=10))
                self.assertFalse(assets.exists())
                configure = subprocess.Popen(
                    [cmake, "-S", str(project), "-B", str(build)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=build_environment,
                )

                deadline = time.monotonic() + 10
                snapshot_reader_waiting = False
                children_path = Path(
                    f"/proc/{configure.pid}/task/{configure.pid}/children"
                )
                while time.monotonic() < deadline and configure.poll() is None:
                    try:
                        child_pids = children_path.read_text(
                            encoding="utf-8"
                        ).split()
                    except FileNotFoundError:
                        child_pids = []
                    for child_pid in child_pids:
                        try:
                            command = Path(f"/proc/{child_pid}/cmdline").read_bytes()
                        except FileNotFoundError:
                            continue
                        if b"rigel_assets.py" in command and b"snapshot" in command:
                            snapshot_reader_waiting = True
                            break
                    if snapshot_reader_waiting:
                        break
                    time.sleep(0.01)

                self.assertTrue(snapshot_reader_waiting)
                self.assertIsNone(configure.poll())
                self.assertFalse(snapshots.exists())
                continue_publication.set()
                configure_output, configure_error = configure.communicate(timeout=15)
                self.assertEqual(
                    configure.returncode,
                    0,
                    configure_output + configure_error,
                )
            finally:
                continue_publication.set()
                if configure is not None and configure.poll() is None:
                    configure.kill()
                    configure.communicate()
                publisher.join(timeout=10)
                if publisher.is_alive():
                    publisher.kill()
                    publisher.join(timeout=10)

            self.assertEqual(publisher.exitcode, 0)
            snapshot = snapshots / str(provenance["output_tree_sha256"])
            self.assertEqual((snapshot / "new.txt").read_bytes(), b"new")
            assembly = (build / "embedded/Dummy_resources.S").read_text(
                encoding="utf-8"
            )
            self.assertIn(str(snapshot / "new.txt"), assembly)
            self.assertNotIn(str(assets), assembly)

            os.replace(assets, root / ".rigel/detached-assets")
            built = subprocess.run(
                [cmake, "--build", str(build), "--target", "Dummy_resources"],
                capture_output=True,
                text=True,
                timeout=15,
                env=build_environment,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)

    def test_cmake_retires_previous_snapshot_after_resource_handoff(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            test_root = Path(directory)
            root = test_root / "asset-root"
            project = test_root / "project"
            build = test_root / "build"
            snapshots = build / "generated-resource-snapshots"
            project.mkdir()
            next_staging, next_provenance = self._prepare_publication(root)
            first_provenance = rigel_assets.read_provenance(root)
            self.assertIsNotNone(first_provenance)
            assert first_provenance is not None
            first_hash = str(first_provenance["output_tree_sha256"])
            abandoned_hash = str(next_provenance["output_tree_sha256"])
            legacy_staging = test_root / "legacy-snapshot"
            rigel_assets.write_output(legacy_staging, "legacy.txt", b"legacy")
            legacy_hash = rigel_assets.sha256_tree(legacy_staging)
            snapshots.mkdir(parents=True)
            os.replace(legacy_staging, snapshots / legacy_hash)
            importer = Path(rigel_assets.__file__).resolve()
            asset_resources = importer.parent.parent / "cmake/AssetResources.cmake"
            wrapper = test_root / "observe_snapshot_handoff.py"
            handoff_observed = test_root / "handoff-observed"
            wrapper.write_text(
                f"""import sys
from pathlib import Path
sys.path.insert(0, {str(importer.parent)!r})
import rigel_assets

arguments = sys.argv[1:]
if "retire-snapshots" in arguments:
    output = Path(arguments[arguments.index("--output") + 1]).resolve()
    retained = Path(arguments[arguments.index("--retain") + 1]).resolve()
    assembly = Path({str(build / "embedded/Dummy_resources.S")!r})
    if not assembly.is_file() or str(retained) not in assembly.read_text(encoding="utf-8"):
        raise SystemExit("snapshot retirement preceded the resource handoff")
    first = output / {first_hash!r}
    if retained.name != {first_hash!r}:
        if not first.is_dir():
            raise SystemExit("previous snapshot was retired before the resource handoff")
        Path({str(handoff_observed)!r}).write_text(retained.name, encoding="utf-8")
raise SystemExit(rigel_assets.main(arguments))
""",
                encoding="utf-8",
            )
            (project / "dummy.cpp").write_text(
                "int generated_resource_dummy() { return 0; }\n",
                encoding="utf-8",
            )

            def write_project(expected_jar_hash: str) -> None:
                (project / "CMakeLists.txt").write_text(
                    f"""cmake_minimum_required(VERSION 3.20)
project(GeneratedResourceRetirement LANGUAGES CXX ASM)
include("{asset_resources}")
add_library(Dummy STATIC dummy.cpp)
rigel_snapshot_generated_resources(
    GENERATED_ROOT
    "{root}"
    "{snapshots}"
    "{sys.executable}"
    "{wrapper}"
    "{expected_jar_hash}"
    "{build / 'embedded/Dummy_resources.S'}")
target_embed_resources(Dummy "${{GENERATED_ROOT}}")
rigel_retire_generated_resource_snapshots(
    "{snapshots}"
    "${{GENERATED_ROOT}}"
    "{sys.executable}"
    "{wrapper}")
""",
                    encoding="utf-8",
                )

            cmake = shutil.which("cmake")
            self.assertIsNotNone(cmake)
            assert cmake is not None
            build_environment = dict(os.environ)
            build_environment["CCACHE_DISABLE"] = "true"
            write_project(str(first_provenance["jar_sha256"]))
            first_configure = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
                env=build_environment,
            )
            self.assertEqual(
                first_configure.returncode,
                0,
                first_configure.stdout + first_configure.stderr,
            )
            self.assertEqual(
                sorted(
                    path.name
                    for path in snapshots.iterdir()
                    if path.is_dir()
                ),
                [first_hash],
            )

            rigel_assets.write_output(next_staging, "new.txt", b"new")
            rigel_assets.publish_generated_tree(
                root, next_staging, next_provenance
            )
            rigel_assets.snapshot_generated_assets(
                root,
                snapshots,
                str(next_provenance["jar_sha256"]),
            )
            self.assertEqual(
                sorted(
                    path.name
                    for path in snapshots.iterdir()
                    if path.is_dir()
                ),
                sorted([first_hash, abandoned_hash]),
            )

            final_staging = root / ".rigel/.assets-staging-final"
            rigel_assets.write_output(final_staging, "final.txt", b"final")
            final_provenance = {
                "generation": "final",
                "jar_sha256": hashlib.sha256(b"final-source").hexdigest(),
                "output_tree_sha256": rigel_assets.sha256_tree(final_staging),
            }
            final_hash = str(final_provenance["output_tree_sha256"])
            rigel_assets.publish_generated_tree(
                root, final_staging, final_provenance
            )
            write_project(str(final_provenance["jar_sha256"]))
            second_configure = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
                env=build_environment,
            )
            self.assertEqual(
                second_configure.returncode,
                0,
                second_configure.stdout + second_configure.stderr,
            )
            self.assertEqual(handoff_observed.read_text(encoding="utf-8"), final_hash)
            self.assertFalse((snapshots / first_hash).exists())
            self.assertFalse((snapshots / abandoned_hash).exists())
            self.assertTrue((snapshots / final_hash).is_dir())
            self.assertEqual(
                sorted(
                    path.name
                    for path in snapshots.iterdir()
                    if path.is_dir()
                ),
                [final_hash],
            )

    def test_cmake_failure_keeps_the_interrupted_handoff_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            test_root = Path(directory)
            root = test_root / "asset-root"
            project = test_root / "project"
            build = test_root / "build"
            snapshots = build / "generated-resource-snapshots"
            assembly = build / "embedded/Dummy_resources.S"
            project.mkdir()
            next_staging, next_provenance = self._prepare_publication(root)
            first_provenance = rigel_assets.read_provenance(root)
            self.assertIsNotNone(first_provenance)
            assert first_provenance is not None
            first_hash = str(first_provenance["output_tree_sha256"])
            second_hash = str(next_provenance["output_tree_sha256"])
            importer = Path(rigel_assets.__file__).resolve()
            asset_resources = importer.parent.parent / "cmake/AssetResources.cmake"
            (project / "dummy.cpp").write_text(
                "int generated_resource_dummy() { return 0; }\n",
                encoding="utf-8",
            )

            def write_project(jar_hash: str, failure: str | None = None) -> None:
                resource_roots = (
                    '"${GENERATED_ROOT}" "${GENERATED_ROOT}"'
                    if failure == "enumeration"
                    else '"${GENERATED_ROOT}"'
                )
                tail = (
                    'message(FATAL_ERROR "injected handoff interruption")'
                    if failure == "handoff"
                    else (
                        ""
                        if failure == "enumeration"
                        else f'''rigel_retire_generated_resource_snapshots(
    "{snapshots}"
    "${{GENERATED_ROOT}}"
    "{sys.executable}"
    "{importer}")'''
                    )
                )
                (project / "CMakeLists.txt").write_text(
                    f"""cmake_minimum_required(VERSION 3.20)
project(InterruptedResourceHandoff LANGUAGES CXX ASM)
include("{asset_resources}")
add_library(Dummy STATIC dummy.cpp)
rigel_snapshot_generated_resources(
    GENERATED_ROOT
    "{root}"
    "{snapshots}"
    "{sys.executable}"
    "{importer}"
    "{jar_hash}"
    "{assembly}")
target_embed_resources(Dummy {resource_roots})
{tail}
""",
                    encoding="utf-8",
                )

            cmake = shutil.which("cmake")
            self.assertIsNotNone(cmake)
            assert cmake is not None
            environment = dict(os.environ)
            environment["CCACHE_DISABLE"] = "true"

            write_project(str(first_provenance["jar_sha256"]))
            configured = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
                env=environment,
            )
            self.assertEqual(
                configured.returncode,
                0,
                configured.stdout + configured.stderr,
            )

            rigel_assets.write_output(next_staging, "new.txt", b"new")
            rigel_assets.publish_generated_tree(
                root, next_staging, next_provenance
            )
            write_project(str(next_provenance["jar_sha256"]), "handoff")
            interrupted = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
                env=environment,
            )
            self.assertNotEqual(interrupted.returncode, 0)
            interrupted_assembly = assembly.read_bytes()
            self.assertIn(str(snapshots / second_hash).encode(), interrupted_assembly)
            self.assertTrue((snapshots / first_hash).is_dir())
            self.assertTrue((snapshots / second_hash).is_dir())

            final_staging = root / ".rigel/.assets-staging-final"
            rigel_assets.write_output(final_staging, "final.txt", b"final")
            final_provenance = {
                "generation": "final",
                "jar_sha256": hashlib.sha256(b"final-source").hexdigest(),
                "output_tree_sha256": rigel_assets.sha256_tree(final_staging),
            }
            final_hash = str(final_provenance["output_tree_sha256"])
            rigel_assets.publish_generated_tree(
                root, final_staging, final_provenance
            )
            write_project(str(final_provenance["jar_sha256"]), "enumeration")
            failed = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
                env=environment,
            )
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("Duplicate logical resource path", failed.stderr)
            self.assertEqual(assembly.read_bytes(), interrupted_assembly)
            self.assertFalse((snapshots / first_hash).exists())
            self.assertTrue((snapshots / second_hash).is_dir())
            self.assertTrue((snapshots / final_hash).is_dir())
            self.assertEqual(
                sorted(
                    path.name
                    for path in snapshots.iterdir()
                    if path.is_dir()
                ),
                sorted([second_hash, final_hash]),
            )

    def test_cmake_rejects_a_generation_replaced_after_synchronization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            test_root = Path(directory)
            root = test_root / "asset-root"
            project = test_root / "project"
            build = test_root / "build"
            snapshots = build / "generated-resource-snapshots"
            first_jar = test_root / "first.jar"
            replacement_jar = test_root / "replacement.jar"
            first_entries = synthetic_block_entries()
            first_entries["base/version.txt"] = b"first"
            replacement_entries = synthetic_block_entries()
            replacement_entries["base/version.txt"] = b"replacement"
            replacement_entries["base/sounds/replacement.ogg"] = b"replacement"
            write_jar(first_jar, first_entries)
            write_jar(replacement_jar, replacement_entries)
            project.mkdir()

            importer = Path(rigel_assets.__file__).resolve()
            asset_resources = importer.parent.parent / "cmake/AssetResources.cmake"
            wrapper = test_root / "replace_after_sync.py"
            wrapper.write_text(
                f"""import sys
from pathlib import Path
sys.path.insert(0, {str(importer.parent)!r})
import rigel_assets

arguments = sys.argv[1:]
if "sync" not in arguments:
    raise SystemExit(rigel_assets.main(arguments))
root = Path(arguments[arguments.index("--root") + 1]).resolve()
jar = Path(arguments[arguments.index("--jar") + 1]).resolve()
provenance, changed = rigel_assets.synchronize(
    root, jar, required_identifiers=("test:stone",))
action = "Synchronized" if changed else "Already current"
print(f"{{action}}: {{root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH}}")
print(f"JAR SHA-256: {{provenance['jar_sha256']}}")
print(f"Output SHA-256: {{provenance['output_tree_sha256']}}")
rigel_assets.synchronize(
    root, Path({str(replacement_jar)!r}),
    required_identifiers=("test:stone",))
""",
                encoding="utf-8",
            )
            (project / "CMakeLists.txt").write_text(
                f"""cmake_minimum_required(VERSION 3.20)
project(GeneratedResourceHandoff NONE)
include("{asset_resources}")
rigel_synchronize_generated_resources(
    GENERATED_ROOT
    GENERATED_JAR_SHA256
    "{root}"
    "{snapshots}"
    "{sys.executable}"
    "{wrapper}"
    "{first_jar}"
    "{build / 'embedded/Dummy_resources.S'}")
""",
                encoding="utf-8",
            )
            cmake = shutil.which("cmake")
            self.assertIsNotNone(cmake)
            assert cmake is not None
            configured = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
            )

            self.assertNotEqual(configured.returncode, 0)
            configure_diagnostic = configured.stdout + configured.stderr
            self.assertIn("published source", configure_diagnostic)
            self.assertIn("JAR changed after synchronization", configure_diagnostic)
            published = rigel_assets.read_provenance(root)
            self.assertIsNotNone(published)
            assert published is not None
            self.assertEqual(published.get("source_version"), "replacement")
            self.assertFalse(snapshots.exists())

    def test_cmake_rejects_source_jar_replaced_after_snapshot_handoff(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            test_root = Path(directory)
            root = test_root / "asset-root"
            project = test_root / "project"
            build = test_root / "build"
            snapshots = build / "generated-resource-snapshots"
            jar_snapshots = build / "synchronized-source-jars"
            source_jar = test_root / "source.jar"
            replacement_jar = test_root / "replacement.jar"
            source_entries = synthetic_block_entries()
            source_entries["base/version.txt"] = b"first"
            replacement_entries = synthetic_block_entries()
            replacement_entries["base/version.txt"] = b"replacement"
            write_jar(source_jar, source_entries)
            write_jar(replacement_jar, replacement_entries)
            source_digest = rigel_assets.sha256_file(source_jar)
            project.mkdir()

            importer = Path(rigel_assets.__file__).resolve()
            asset_resources = importer.parent.parent / "cmake/AssetResources.cmake"
            wrapper = test_root / "replace_after_handoff.py"
            wrapper.write_text(
                f"""import sys
from pathlib import Path
sys.path.insert(0, {str(importer.parent)!r})
import rigel_assets

arguments = sys.argv[1:]
if "sync" in arguments:
    root = Path(arguments[arguments.index("--root") + 1]).resolve()
    jar = Path(arguments[arguments.index("--jar") + 1]).resolve()
    provenance, changed = rigel_assets.synchronize(
        root, jar, required_identifiers=("test:stone",))
    action = "Synchronized" if changed else "Already current"
    print(f"{{action}}: {{root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH}}")
    print(f"JAR SHA-256: {{provenance['jar_sha256']}}")
    print(f"Output SHA-256: {{provenance['output_tree_sha256']}}")
    raise SystemExit(0)

result = rigel_assets.main(arguments)
if result == 0 and "snapshot" in arguments:
    source = Path({str(source_jar)!r})
    replacement = Path({str(replacement_jar)!r})
    source.write_bytes(replacement.read_bytes())
raise SystemExit(result)
""",
                encoding="utf-8",
            )
            (project / "CMakeLists.txt").write_text(
                f"""cmake_minimum_required(VERSION 3.20)
project(SynchronizedSourceJarHandoff NONE)
include("{asset_resources}")
rigel_synchronize_generated_resources(
    GENERATED_ROOT
    GENERATED_JAR_SHA256
    "{root}"
    "{snapshots}"
    "{sys.executable}"
    "{wrapper}"
    "{source_jar}"
    "{build / 'embedded/Dummy_resources.S'}")
if(NOT GENERATED_JAR_SHA256 STREQUAL "{source_digest}")
    message(FATAL_ERROR "Synchronization digest output changed")
endif()
rigel_snapshot_synchronized_source_jar(
    SNAPSHOT_JAR
    "{source_jar}"
    "${{GENERATED_JAR_SHA256}}"
    "{jar_snapshots}")
""",
                encoding="utf-8",
            )
            cmake = shutil.which("cmake")
            self.assertIsNotNone(cmake)
            assert cmake is not None
            configured = subprocess.run(
                [cmake, "-S", str(project), "-B", str(build)],
                capture_output=True,
                text=True,
                timeout=15,
            )

            self.assertNotEqual(configured.returncode, 0)
            diagnostic = configured.stdout + configured.stderr
            self.assertIn(
                "Source JAR changed after asset synchronization and snapshot handoff",
                diagnostic,
            )
            self.assertTrue(any(snapshots.iterdir()))
            self.assertFalse(jar_snapshots.exists() and any(jar_snapshots.iterdir()))

    def test_output_path_rejects_escape(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(rigel_assets.AssetImportError):
                rigel_assets.write_output(Path(directory), "../escape", b"bad")


class DirectExtractionTest(unittest.TestCase):
    def test_extracts_binary_assets_and_normalizes_entity_references(self) -> None:
        model = {
            "texture_width": 16,
            "texture_height": 16,
            "textures": {"diffuse": "base:textures/entities/test.png"},
            "bones": [],
        }
        animation = {"animations": {"idle": {"loop": True, "bones": {}}}}
        entries = {
            "base/textures/entities/test.png": b"synthetic-png",
            "base/models/entities/test.json": json.dumps(model).encode(),
            "base/animations/entities/test.animation.json": json.dumps(animation).encode(),
            "base/sounds/test.ogg": b"synthetic-ogg",
            "base/textures/source.xcf": b"not-runtime-content",
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            staging = root / "output"
            with zipfile.ZipFile(jar) as archive:
                counts = rigel_assets.extract_direct_assets(
                    archive, rigel_assets.indexed_archive(archive), staging
                )

            self.assertEqual(
                counts, {"textures": 1, "models": 1, "animations": 1, "sounds": 1}
            )
            self.assertEqual(
                (staging / "textures/entities/test.png").read_bytes(), b"synthetic-png"
            )
            self.assertEqual((staging / "sounds/test.ogg").read_bytes(), b"synthetic-ogg")
            normalized_model = json.loads(
                (staging / "models/entities/test.json").read_text(encoding="utf-8")
            )
            self.assertEqual(normalized_model["id"], "test")
            self.assertEqual(normalized_model["lighting"], "unlit")
            self.assertEqual(normalized_model["model_scale"], 0.0625)
            self.assertEqual(
                normalized_model["textures"]["diffuse"], "textures/entities/test.png"
            )
            normalized_animation = json.loads(
                (staging / "animations/entities/test.animation.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(normalized_animation["id"], "test")
            self.assertFalse((staging / "textures/source.xcf").exists())

    def test_relaxed_json_handles_comments_trailing_commas_and_legacy_gap(self) -> None:
        parsed = rigel_assets.load_relaxed_json(
            b'{"text": "} {,]", "items": [{"a": 1,} /* source defect */ {"b": 2}]}',
            "fixture",
        )
        self.assertEqual(
            parsed, {"text": "} {,]", "items": [{"a": 1}, {"b": 2}]}
        )

    def test_entity_texture_outside_base_namespace_fails_closed(self) -> None:
        model = {
            "textures": {"diffuse": "other:textures/entities/test.png"},
            "bones": [],
        }
        with self.assertRaisesRegex(rigel_assets.AssetImportError, "expected a base:"):
            rigel_assets.normalize_entity_model(
                json.dumps(model).encode(), "fixture/model.json", "test"
            )

    def test_normalized_direct_asset_paths_cannot_overwrite_each_other(self) -> None:
        model = lambda texture: encoded_json(
            {"textures": {"diffuse": texture}, "bones": []}
        )
        entries = {
            "base/models/entities/shared.json": model(
                "base:textures/entities/shared.png"
            ),
            "base/models/entities/planets/shared.json": model(
                "base:textures/entities/planets/shared.png"
            ),
            "base/textures/entities/shared.png": synthetic_png(),
            "base/textures/entities/planets/shared.png": synthetic_png(),
        }
        with tempfile.TemporaryDirectory() as directory:
            jar = Path(directory) / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive, self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "duplicate generated logical path.*models/entities/shared.json",
            ):
                rigel_assets.extract_direct_assets(
                    archive,
                    rigel_assets.indexed_archive(archive),
                    Path(directory) / "output",
                )


class BlockCompilerTest(unittest.TestCase):
    def test_compiles_explicit_and_included_generated_states(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                count = rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), output
                )

            self.assertEqual(count, 4)
            base = (output / "blocks/test__stone.yaml").read_text(encoding="utf-8")
            self.assertIn('id: "test:stone"', base)
            self.assertIn('all: "textures/blocks/test_stone.png"', base)
            generated = (
                output / "blocks/test__stone[shape=full].yaml"
            ).read_text(encoding="utf-8")
            self.assertIn('id: "test:stone[shape=full]"', generated)
            self.assertIn("opaque: false", generated)
            self.assertIn("light_attenuation: 2", generated)
            bright = (
                output / "blocks/test__stone[kind=bright].yaml"
            ).read_text(encoding="utf-8")
            self.assertIn("solid: false", bright)
            self.assertIn("emits_light: 7", bright)

    def test_compiles_one_cuboid_for_base_and_generated_states(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, single_cuboid_fixture.entries())
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                indexed = rigel_assets.indexed_archive(archive)
                rigel_assets.extract_direct_assets(archive, indexed, output)
                count = rigel_assets.compile_blocks(archive, indexed, output)

            self.assertEqual(count, 2)
            model = rigel_assets.parse_generated_model(
                (output / "models/blocks/ledge.yaml").read_bytes(),
                "models/blocks/ledge.yaml",
            )
            self.assertEqual(model["id"], "base:block_model/ledge")
            self.assertEqual(len(model["cuboids"]), 1)
            cuboid = model["cuboids"][0]
            self.assertEqual(
                cuboid["bounds"],
                [-0.15625, -0.03125, 0.21875, 1.15625, 0.65625, 0.78125],
            )
            self.assertEqual(set(cuboid["faces"]), {"pos_x", "neg_y"})
            self.assertEqual(
                cuboid["faces"]["pos_x"]["uv"],
                [0.9375, 0.125, 0.1875, 0.875],
            )
            self.assertEqual(cuboid["faces"]["pos_x"]["rotation"], 90)
            self.assertFalse(
                cuboid["faces"]["pos_x"]["ambient_occlusion"]
            )
            self.assertFalse(cuboid["faces"]["pos_x"]["cull"])
            base = rigel_assets.parse_generated_block(
                (output / "blocks/test__ledge.yaml").read_bytes(), "base.yaml"
            )
            generated = rigel_assets.parse_generated_block(
                (
                    output / "blocks/test__ledge[facing=east].yaml"
                ).read_bytes(),
                "generated.yaml",
            )
            self.assertEqual(base["model"], "base:block_model/ledge")
            self.assertEqual(generated["model"], "base:block_model/ledge")
            self.assertEqual(generated["orientation"], [0, 90, 0])
            self.assertEqual(generated["textures"], base["textures"])
            counts = rigel_assets.validate_generated_tree(
                output,
                required_identifiers=(
                    "test:ledge",
                    "test:ledge[facing=east]",
                ),
            )
            self.assertEqual(counts["block_models"], 1)

    def test_nontransparent_partial_model_keeps_opaque_render_layer(self) -> None:
        entries = single_cuboid_fixture.entries()
        model = json.loads(entries["base/models/blocks/ledge.json"])
        model["isTransparent"] = False
        entries["base/models/blocks/ledge.json"] = encoded_json(model)
        block = json.loads(entries["base/blocks/ledge.json"])
        block["defaultProperties"]["isOpaque"] = False
        entries["base/blocks/ledge.json"] = encoded_json(block)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), root
                )

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__ledge.yaml").read_bytes(),
                "blocks/test__ledge.yaml",
            )
            self.assertEqual(generated["model"], "base:block_model/ledge")
            self.assertFalse(generated["opaque"])
            self.assertEqual(generated["layer"], "opaque")

    def test_nontransparent_partial_model_with_binary_alpha_uses_cutout_layer(self) -> None:
        entries = single_cuboid_fixture.entries()
        block = json.loads(entries["base/blocks/ledge.json"])
        block["defaultProperties"]["isOpaque"] = False
        entries["base/blocks/ledge.json"] = encoded_json(block)
        entries["base/textures/blocks/ledge.png"] = synthetic_png(
            transparent_pixels={(0, 0), (8, 8)}
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), root
                )

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__ledge.yaml").read_bytes(),
                "blocks/test__ledge.yaml",
            )
            self.assertEqual(generated["model"], "base:block_model/ledge")
            self.assertFalse(generated["opaque"])
            self.assertEqual(generated["layer"], "cutout")

    def test_transparent_model_with_binary_alpha_uses_cutout_layer(self) -> None:
        entries = single_cuboid_fixture.entries()
        model = json.loads(entries["base/models/blocks/ledge.json"])
        model["isTransparent"] = True
        entries["base/models/blocks/ledge.json"] = encoded_json(model)
        entries["base/textures/blocks/ledge.png"] = synthetic_png(
            transparent_pixels={(0, 0), (8, 8)}
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), root
                )

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__ledge.yaml").read_bytes(),
                "blocks/test__ledge.yaml",
            )
            self.assertFalse(generated["opaque"])
            self.assertEqual(generated["layer"], "cutout")

    def test_fractional_alpha_uses_transparent_layer(self) -> None:
        entries = single_cuboid_fixture.entries()
        block = json.loads(entries["base/blocks/ledge.json"])
        block["defaultProperties"]["isOpaque"] = False
        entries["base/blocks/ledge.json"] = encoded_json(block)
        entries["base/textures/blocks/ledge.png"] = synthetic_png(
            fractional_pixels={(0, 0): 64, (8, 8): 192}
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), root
                )

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__ledge.yaml").read_bytes(),
                "blocks/test__ledge.yaml",
            )
            self.assertEqual(generated["layer"], "transparent")

    def test_refractive_binary_alpha_explicitly_blends(self) -> None:
        entries = single_cuboid_fixture.entries()
        block = json.loads(entries["base/blocks/ledge.json"])
        block["defaultProperties"].update(
            {"isOpaque": False, "refractiveIndex": 1.6}
        )
        entries["base/blocks/ledge.json"] = encoded_json(block)
        entries["base/textures/blocks/ledge.png"] = synthetic_png(
            transparent_pixels={(0, 0), (8, 8)}
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), root
                )

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__ledge.yaml").read_bytes(),
                "blocks/test__ledge.yaml",
            )
            self.assertEqual(generated["layer"], "transparent")

    def test_fractional_mixed_model_preserves_per_texture_materials(self) -> None:
        entries = synthetic_cuboid_entries()
        model = json.loads(entries["base/models/blocks/post.json"])
        model["isTransparent"] = False
        entries["base/models/blocks/post.json"] = encoded_json(model)
        block = json.loads(entries["base/blocks/test_post.json"])
        block["defaultProperties"]["isOpaque"] = False
        entries["base/blocks/test_post.json"] = encoded_json(block)
        entries["base/textures/blocks/red_accent.png"] = synthetic_png(
            fractional_pixels={(0, 0): 64, (8, 8): 192}
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), root
                )

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__post.yaml").read_bytes(),
                "blocks/test__post.yaml",
            )
            self.assertEqual(generated["layer"], "opaque")
            self.assertEqual(
                generated["texture_render_layers"],
                {"accent": "transparent"},
            )

    def test_refractive_mixed_model_promotes_only_binary_alpha_slot(self) -> None:
        entries = synthetic_cuboid_entries()
        model = json.loads(entries["base/models/blocks/post.json"])
        model["isTransparent"] = False
        entries["base/models/blocks/post.json"] = encoded_json(model)
        block = json.loads(entries["base/blocks/test_post.json"])
        block["defaultProperties"].update(
            {"isOpaque": False, "refractiveIndex": 1.6}
        )
        entries["base/blocks/test_post.json"] = encoded_json(block)
        entries["base/textures/blocks/red_accent.png"] = synthetic_png(
            transparent_pixels={(0, 0), (8, 8)}
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                indexed = rigel_assets.indexed_archive(archive)
                rigel_assets.extract_direct_assets(archive, indexed, root)
                rigel_assets.compile_blocks(archive, indexed, root)

            generated = rigel_assets.parse_generated_block(
                (root / "blocks/test__post.yaml").read_bytes(),
                "blocks/test__post.yaml",
            )
            self.assertEqual(generated["layer"], "opaque")
            self.assertEqual(
                generated["texture_render_layers"],
                {"accent": "transparent"},
            )
            effective_layers = {
                slot: generated.get("texture_render_layers", {}).get(
                    slot, generated["layer"]
                )
                for slot in generated["textures"]
            }
            self.assertEqual(
                effective_layers,
                {"accent": "transparent", "surface": "opaque"},
            )
            alpha_classes = {
                slot: rigel_assets.classify_png_alpha(
                    (root / texture).read_bytes(), texture
                )
                for slot, texture in generated["textures"].items()
            }
            self.assertEqual(
                alpha_classes,
                {
                    "accent": rigel_assets.TextureAlphaClass.BINARY,
                    "surface": rigel_assets.TextureAlphaClass.FULLY_OPAQUE,
                },
            )
            rigel_assets.validate_generated_tree(
                root, required_identifiers=("test:post",)
            )

    def test_preserves_generator_orientation_and_top_bottom_uv_behavior(self) -> None:
        entries = synthetic_block_entries()
        generators = json.loads(
            entries["base/block_state_generators/variants.json"]
        )
        leaf = generators["generators"][1]
        leaf["overrides"].update(
            {"rotation": [0, 0, 90], "rotateTopBottom": True}
        )
        entries["base/block_state_generators/variants.json"] = encoded_json(
            generators
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), output
                )

            generated = rigel_assets.parse_generated_block(
                (output / "blocks/test__stone[shape=full].yaml").read_bytes(),
                "generated.yaml",
            )
            self.assertEqual(generated["orientation"], [0, 0, 90])
            self.assertTrue(generated["rotate_top_bottom"])

    def test_emits_source_face_textures_for_runtime_orientation(self) -> None:
        output = rigel_assets.render_block_yaml(
            "test:directional",
            {
                "modelName": "unused",
                "rotation": [0, 0, 90],
                "rotateTopBottom": True,
            },
            _DirectionalModelResolver(),
            "fixture",
        )
        block = rigel_assets.parse_generated_block(output, "generated.yaml")

        self.assertEqual(block["orientation"], [0, 0, 90])
        self.assertTrue(block["rotate_top_bottom"])
        self.assertEqual(
            block["textures"],
            {
                "pos_x": "textures/blocks/pos_x.png",
                "neg_x": "textures/blocks/neg_x.png",
                "pos_y": "textures/blocks/pos_y.png",
                "neg_y": "textures/blocks/neg_y.png",
                "pos_z": "textures/blocks/pos_z.png",
                "neg_z": "textures/blocks/neg_z.png",
            },
        )

    def test_rejects_unmeasured_and_composed_source_rotations(self) -> None:
        cases = ([180, 0, 0], [0, 0, 270], [90, 90, 0], [0, 45, 0])
        for rotation in cases:
            with self.subTest(rotation=rotation), self.assertRaisesRegex(
                rigel_assets.AssetImportError, "supported block-state set"
            ):
                rigel_assets.render_block_yaml(
                    "test:bad",
                    {"modelName": "unused", "rotation": rotation},
                    _DirectionalModelResolver(),
                    "fixture",
                )

        with self.assertRaisesRegex(
            rigel_assets.AssetImportError, "requires X90 or Z90"
        ):
            rigel_assets.render_block_yaml(
                "test:bad",
                {"modelName": "unused", "rotateTopBottom": True},
                _DirectionalModelResolver(),
                "fixture",
            )

    def test_compiles_inherited_cuboids_and_reuses_normalized_geometry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_cuboid_entries())
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                indexed = rigel_assets.indexed_archive(archive)
                rigel_assets.extract_direct_assets(archive, indexed, output)
                count = rigel_assets.compile_blocks(
                    archive, indexed, output
                )

            self.assertEqual(count, 3)
            model_path = output / "models/blocks/post.yaml"
            model = rigel_assets.parse_generated_model(
                model_path.read_bytes(), "models/blocks/post.yaml"
            )
            self.assertEqual(model["id"], "base:block_model/post")
            self.assertEqual(model["texture_slots"], ["accent", "surface"])
            self.assertEqual(len(model["cuboids"]), 2)
            first = model["cuboids"][0]
            self.assertEqual(
                first["bounds"],
                [-0.09375, -0.03125, 0.21875, 1.09375, 0.53125, 0.78125],
            )
            self.assertEqual(set(first["faces"]), {"pos_x"})
            face = first["faces"]["pos_x"]
            self.assertEqual(face["uv"], [0.75, 0.125, 0.25, 0.875])
            self.assertEqual(face["rotation"], 270)
            self.assertTrue(face["ambient_occlusion"])
            self.assertFalse(face["cull"])
            self.assertEqual(face["shading"], "pos_y")
            self.assertEqual(
                model["cuboids"][1]["faces"]["neg_y"]["uv"],
                [0.9375, 0.0, 1.0, 0.0],
            )
            self.assertFalse(
                (output / "models/blocks/red_post.yaml").exists()
            )
            self.assertFalse(
                (output / "models/blocks/blue_post.yaml").exists()
            )

            red = rigel_assets.parse_generated_block(
                (output / "blocks/test__post.yaml").read_bytes(), "red.yaml"
            )
            generated = rigel_assets.parse_generated_block(
                (output / "blocks/test__post[shape=generated].yaml").read_bytes(),
                "generated.yaml",
            )
            blue = rigel_assets.parse_generated_block(
                (output / "blocks/test__post[color=blue].yaml").read_bytes(),
                "blue.yaml",
            )
            for block in (red, generated, blue):
                self.assertEqual(block["model"], "base:block_model/post")
                self.assertFalse(block["opaque"])
                self.assertTrue(block["cull_same_type"])
            self.assertEqual(
                red["textures"],
                {
                    "accent": "textures/blocks/red_accent.png",
                    "surface": "textures/blocks/red_surface.png",
                },
            )
            self.assertEqual(generated["textures"], red["textures"])
            self.assertEqual(generated["orientation"], [0, 90, 0])
            self.assertEqual(
                red["collision"],
                {
                    "boxes": [
                        [-0.09375, -0.03125, 0.21875,
                         1.09375, 0.53125, 0.78125],
                        [0.25, 0.5, 0.25, 0.75, 1.0, 0.75],
                    ]
                },
            )
            self.assertEqual(
                generated["collision"],
                {
                    "boxes": [
                        [0.21875, -0.03125, -0.09375,
                         0.78125, 0.53125, 1.09375],
                        [0.25, 0.5, 0.25, 0.75, 1.0, 0.75],
                    ]
                },
            )
            self.assertEqual(
                blue["textures"]["surface"],
                "textures/blocks/blue_surface.png",
            )
            counts = rigel_assets.validate_generated_tree(
                output, required_identifiers=("test:post",)
            )
            self.assertEqual(counts["block_models"], 1)

    def test_empty_geometry_remains_builtin_none(self) -> None:
        entries = {
            "base/models/blocks/empty.json": encoded_json({"cuboids": []}),
            "base/blocks/empty.json": encoded_json(
                {
                    "stringId": "test:empty",
                    "defaultProperties": {
                        "modelName": "base:models/blocks/empty.json"
                    },
                    "blockStates": {"default": {}},
                }
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), output
                )
            block = rigel_assets.parse_generated_block(
                (output / "blocks/test__empty.yaml").read_bytes(), "empty.yaml"
            )
            self.assertEqual(block["model"], "none")
            self.assertFalse((output / "models/blocks/empty.yaml").exists())

    def test_explicit_planes_are_omitted_with_precise_diagnostic(self) -> None:
        entries = synthetic_cuboid_entries()
        model = json.loads(entries["base/models/blocks/post.json"])
        model["planes"] = [
            {
                "vertices": [0, 0, 0, 16, 0, 0, 16, 16, 0, 0, 16, 0],
                "texture": "surface",
            }
        ]
        entries["base/models/blocks/post.json"] = encoded_json(model)
        diagnostics: list[str] = []
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                count = rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), output,
                    diagnostics,
                )

            self.assertEqual(count, 0)
            self.assertFalse(any((output / "blocks").glob("*.yaml")))
            self.assertFalse((output / "models/blocks/post.yaml").exists())
            self.assertEqual(len(diagnostics), 1)
            self.assertIn("3 block states", diagnostics[0])
            self.assertIn("explicit plane geometry", diagnostics[0])
            self.assertIn("base/models/blocks/post.json", diagnostics[0])

    def test_malformed_cuboid_fields_fail_closed(self) -> None:
        cases = {
            "finite number": lambda cuboid, face: cuboid.update(
                {"localBounds": [False, 0, 0, 16, 16, 16]}
            ),
            "inflate": lambda cuboid, face: cuboid.update({"inflate": "wide"}),
            "four coordinates": lambda cuboid, face: face.update({"uv": [0, 1, 2]}),
            "quarter turn": lambda cuboid, face: face.update({"uvRotation": 45}),
            "ambientocclusion": lambda cuboid, face: face.update(
                {"ambientocclusion": "yes"}
            ),
            "cullFace": lambda cuboid, face: face.update({"cullFace": 1}),
            "shadingFace": lambda cuboid, face: face.update(
                {"shadingFace": "localDiagonal"}
            ),
        }
        for message, mutate in cases.items():
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                entries = synthetic_cuboid_entries()
                model = json.loads(entries["base/models/blocks/post.json"])
                cuboid = model["cuboids"][0]
                face = cuboid["faces"]["localPosX"]
                mutate(cuboid, face)
                entries["base/models/blocks/post.json"] = encoded_json(model)
                jar = Path(directory) / "fixture.jar"
                write_jar(jar, entries)
                with zipfile.ZipFile(jar) as archive, self.assertRaisesRegex(
                    rigel_assets.AssetImportError, message
                ):
                    rigel_assets.compile_blocks(
                        archive,
                        rigel_assets.indexed_archive(archive),
                        Path(directory) / "output",
                    )

    def test_unknown_meaningful_block_field_fails_closed(self) -> None:
        entries = synthetic_block_entries()
        block = json.loads(entries["base/blocks/test_stone.json"])
        block["blockStates"]["default"]["unknownCollisionMode"] = "ghost"
        entries["base/blocks/test_stone.json"] = encoded_json(block)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                with self.assertRaisesRegex(
                    rigel_assets.AssetImportError, "unknownCollisionMode"
                ):
                    rigel_assets.compile_blocks(
                        archive, rigel_assets.indexed_archive(archive), root / "output"
                    )

    def test_snapshots_walk_through_and_full_cube_collision(self) -> None:
        audit = rigel_assets.BlockCollisionImportAudit()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive,
                    rigel_assets.indexed_archive(archive),
                    output,
                    collision_audit=audit,
                )

            base = rigel_assets.parse_generated_block(
                (output / "blocks/test__stone.yaml").read_bytes(), "base.yaml"
            )
            walk_through = rigel_assets.parse_generated_block(
                (output / "blocks/test__stone[kind=bright].yaml").read_bytes(),
                "walk-through.yaml",
            )

        self.assertEqual(base["collision"], "full")
        self.assertEqual(base["collision_provenance"], "exact")
        self.assertEqual(walk_through["model"], "cube")
        self.assertEqual(walk_through["collision"], "none")
        self.assertEqual(walk_through["collision_provenance"], "exact")
        self.assertEqual(
            audit.provenance(),
            {
                "schema": rigel_assets.BLOCK_COLLISION_SUPPORT_SCHEMA,
                "empty": 2,
                "full": 2,
                "single_partial": 0,
                "multi_box": 0,
                "exact_derived": 4,
                "conservative_fallback": 0,
                "ambiguous": 0,
            },
        )

    def test_snapshots_partial_multiple_helper_and_overhanging_cuboids(self) -> None:
        def model(cuboids: list[dict[str, object]]) -> bytes:
            return encoded_json(
                {
                    "textures": {
                        "all": {
                            "fileName": "base:textures/blocks/collision.png"
                        }
                    },
                    "cuboids": cuboids,
                }
            )

        def cuboid(
            bounds: list[int], face: str = "localPosY"
        ) -> dict[str, object]:
            return {
                "localBounds": bounds,
                "faces": {
                    face: {"uv": [0, 0, 16, 16], "texture": "all"}
                },
            }

        entries = {
            "base/models/blocks/slab.json": model(
                [cuboid([0, 0, 0, 16, 8, 16])]
            ),
            "base/models/blocks/stair.json": model(
                [
                    cuboid([0, 0, 0, 16, 8, 16]),
                    cuboid([0, 8, 0, 8, 16, 16]),
                    cuboid([0, 8, 0, 0, 16, 16], "localPosX"),
                ]
            ),
            "base/models/blocks/overhang.json": model(
                [cuboid([-4, 0, 4, 20, 16, 12])]
            ),
            "base/textures/blocks/collision.png": synthetic_png(),
        }
        for name in ("slab", "stair", "overhang"):
            entries[f"base/blocks/{name}.json"] = encoded_json(
                {
                    "stringId": f"test:{name}",
                    "defaultProperties": {
                        "modelName": f"base:models/blocks/{name}.json"
                    },
                    "blockStates": {"default": {}},
                }
            )

        audit = rigel_assets.BlockCollisionImportAudit()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive,
                    rigel_assets.indexed_archive(archive),
                    output,
                    collision_audit=audit,
                )
            collisions = {
                name: rigel_assets.parse_generated_block(
                    (output / f"blocks/test__{name}.yaml").read_bytes(),
                    f"{name}.yaml",
                )["collision"]
                for name in ("slab", "stair", "overhang")
            }

        self.assertEqual(
            collisions["slab"],
            {"boxes": [[0.0, 0.0, 0.0, 1.0, 0.5, 1.0]]},
        )
        self.assertEqual(
            collisions["stair"],
            {
                "boxes": [
                    [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                    [0.0, 0.5, 0.0, 0.5, 1.0, 1.0],
                ]
            },
        )
        self.assertEqual(
            collisions["overhang"],
            {"boxes": [[-0.25, 0.0, 0.25, 1.25, 1.0, 0.75]]},
        )
        report = audit.provenance()
        self.assertEqual(report["single_partial"], 2)
        self.assertEqual(report["multi_box"], 1)
        self.assertEqual(report["exact_derived"], 3)

    def test_bounds_imported_collision_box_cardinality(self) -> None:
        def entries(box_count: int) -> dict[str, bytes]:
            cuboids = [
                {
                    "localBounds": [index / 4.0, 0, 0, 16, 16, 16],
                    "faces": {
                        "localPosY": {
                            "uv": [0, 0, 16, 16],
                            "texture": "all",
                        }
                    },
                }
                for index in range(box_count)
            ]
            return {
                "base/models/blocks/cardinality.json": encoded_json(
                    {
                        "textures": {
                            "all": {
                                "fileName": (
                                    "base:textures/blocks/cardinality.png"
                                )
                            }
                        },
                        "cuboids": cuboids,
                    }
                ),
                "base/blocks/cardinality.json": encoded_json(
                    {
                        "stringId": "test:cardinality",
                        "defaultProperties": {
                            "modelName": (
                                "base:models/blocks/cardinality.json"
                            )
                        },
                        "blockStates": {"default": {}},
                    }
                ),
                "base/textures/blocks/cardinality.png": synthetic_png(),
            }

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "boundary.jar"
            write_jar(jar, entries(rigel_assets.BLOCK_COLLISION_MAXIMUM_BOXES))
            output = root / "boundary"
            with zipfile.ZipFile(jar) as archive:
                rigel_assets.compile_blocks(
                    archive, rigel_assets.indexed_archive(archive), output
                )
            block = rigel_assets.parse_generated_block(
                (output / "blocks/test__cardinality.yaml").read_bytes(),
                "blocks/test__cardinality.yaml",
            )
            self.assertEqual(
                len(block["collision"]["boxes"]),
                rigel_assets.BLOCK_COLLISION_MAXIMUM_BOXES,
            )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "excessive.jar"
            write_jar(
                jar,
                entries(rigel_assets.BLOCK_COLLISION_MAXIMUM_BOXES + 1),
            )
            with zipfile.ZipFile(jar) as archive, self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "collision shape has 17 positive-volume boxes; at most 16",
            ):
                rigel_assets.compile_blocks(
                    archive,
                    rigel_assets.indexed_archive(archive),
                    root / "excessive",
                )

    def test_orients_collision_bounds_for_every_supported_state_turn(self) -> None:
        source = (-0.25, 0.125, 0.25, 0.75, 0.625, 1.25)
        cases = {
            (0, 0, 0): source,
            (90, 0, 0): (-0.25, 0.25, 0.375, 0.75, 1.25, 0.875),
            (270, 0, 0): (-0.25, -0.25, 0.125, 0.75, 0.75, 0.625),
            (0, 90, 0): (-0.25, 0.125, -0.25, 0.75, 0.625, 0.75),
            (0, 180, 0): (0.25, 0.125, -0.25, 1.25, 0.625, 0.75),
            (0, 270, 0): (0.25, 0.125, 0.25, 1.25, 0.625, 1.25),
            (0, 0, 90): (0.125, 0.25, 0.25, 0.625, 1.25, 1.25),
        }
        for orientation, expected in cases.items():
            with self.subTest(orientation=orientation):
                self.assertEqual(
                    rigel_assets._oriented_collision_bounds(
                        source, orientation
                    ),
                    expected,
                )

    def test_collision_bounds_outside_supported_overhang_fail_closed(self) -> None:
        entries = synthetic_cuboid_entries()
        model = json.loads(entries["base/models/blocks/post.json"])
        model["cuboids"][0]["localBounds"][0] = -5
        entries["base/models/blocks/post.json"] = encoded_json(model)
        with tempfile.TemporaryDirectory() as directory:
            jar = Path(directory) / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive, self.assertRaisesRegex(
                rigel_assets.AssetImportError, "supported.*range"
            ):
                rigel_assets.compile_blocks(
                    archive,
                    rigel_assets.indexed_archive(archive),
                    Path(directory) / "output",
                )

    def test_collision_provenance_requires_exact_published_shapes(self) -> None:
        audit = rigel_assets.BlockCollisionImportAudit()
        audit.record("test:exact", rigel_assets.ResolvedCollisionShape("empty"))
        report = audit.provenance()
        self.assertEqual(report["exact_derived"], 1)
        self.assertEqual(report["conservative_fallback"], 0)
        self.assertEqual(report["ambiguous"], 0)
        rigel_assets.validate_block_collision_import_report(
            report,
            {
                "empty": 1,
                "full": 0,
                "single_partial": 0,
                "multi_box": 0,
                "exact_derived": 1,
                "conservative_fallback": 0,
                "ambiguous": 0,
            },
        )

        plausible_fallback = dict(report)
        plausible_fallback["exact_derived"] = 0
        plausible_fallback["conservative_fallback"] = 1
        plausible_fallback["ambiguous"] = 1
        with self.assertRaisesRegex(
            rigel_assets.AssetImportError, "must be exact derivations"
        ):
            rigel_assets.validate_block_collision_import_report(
                plausible_fallback
            )

    def test_model_transparency_and_fluid_do_not_blend_opaque_texture(self) -> None:
        resolver = _TransparentModelResolver()
        output = rigel_assets.render_block_yaml(
            "test:fluid",
            {"modelName": "unused", "isFluid": True},
            resolver,
            "fixture",
        ).decode()
        self.assertIn("opaque: false", output)
        self.assertIn("layer: opaque", output)
        self.assertIn("cull_same_type: true", output)

    def test_missing_generator_reference_fails_closed(self) -> None:
        entries = synthetic_block_entries()
        del entries["base/block_state_generators/variants.json"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            with zipfile.ZipFile(jar) as archive:
                with self.assertRaisesRegex(
                    rigel_assets.AssetImportError, "missing state generator"
                ):
                    rigel_assets.compile_blocks(
                        archive, rigel_assets.indexed_archive(archive), root / "output"
                    )

    def test_unsupported_block_texture_dimensions_are_omitted_visibly(self) -> None:
        entries = synthetic_block_entries()
        entries["base/textures/blocks/test_stone.png"] = synthetic_png(64, 16)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            output = root / "output"
            diagnostics: list[str] = []
            with zipfile.ZipFile(jar) as archive:
                indexed = rigel_assets.indexed_archive(archive)
                rigel_assets.extract_direct_assets(archive, indexed, output)
                rigel_assets.compile_blocks(archive, indexed, output)

            omitted = rigel_assets.omit_blocks_with_unsupported_textures(
                output, diagnostics
            )

            self.assertEqual(omitted, 4)
            self.assertFalse(any((output / "blocks").glob("*.yaml")))
            self.assertEqual(len(diagnostics), 1)
            self.assertIn("64x16", diagnostics[0])

    def test_texture_omission_prunes_unreferenced_normalized_models(self) -> None:
        entries = synthetic_cuboid_entries()
        for path in list(entries):
            if path.startswith("base/textures/blocks/"):
                entries[path] = synthetic_png(64, 16)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)
            output = root / "output"
            with zipfile.ZipFile(jar) as archive:
                indexed = rigel_assets.indexed_archive(archive)
                rigel_assets.extract_direct_assets(archive, indexed, output)
                rigel_assets.compile_blocks(archive, indexed, output)

            omitted = rigel_assets.omit_blocks_with_unsupported_textures(output)

            self.assertEqual(omitted, 3)
            self.assertFalse(any((output / "blocks").glob("*.yaml")))
            self.assertFalse((output / "models/blocks/post.yaml").exists())


class GeneratedTreeClosureTest(unittest.TestCase):
    def test_audits_alpha_classes_effective_layers_and_cross_classifications(
        self,
    ) -> None:
        def generated_block(
            identifier: str,
            layer: str,
            textures: dict[str, str] | None = None,
            overrides: dict[str, str] | None = None,
        ) -> bytes:
            lines = [
                f"id: {json.dumps(identifier)}",
                "model: test:synthetic",
                "opaque: false",
                "solid: false",
                "collision: none",
                "collision_provenance: exact",
                f"layer: {layer}",
            ]
            if overrides:
                lines.append("texture_render_layers:")
                lines.extend(
                    f"  {slot}: {json.dumps(value)}"
                    for slot, value in overrides.items()
                )
            if textures:
                lines.append("textures:")
                lines.extend(
                    f"  {slot}: {json.dumps(value)}"
                    for slot, value in textures.items()
                )
            return ("\n".join(lines) + "\n").encode("utf-8")

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            texture_paths = {
                "opaque": "textures/blocks/opaque.png",
                "binary": "textures/blocks/binary.png",
                "fractional": "textures/blocks/fractional.png",
            }
            rigel_assets.write_output(
                output, texture_paths["opaque"], synthetic_png()
            )
            rigel_assets.write_output(
                output,
                texture_paths["binary"],
                synthetic_png(transparent_pixels={(0, 0)}),
            )
            rigel_assets.write_output(
                output,
                texture_paths["fractional"],
                synthetic_png(fractional_pixels={(0, 0): 127}),
            )
            fixtures = {
                "opaque": generated_block(
                    "test:opaque", "opaque", {"all": texture_paths["opaque"]}
                ),
                "cutout": generated_block(
                    "test:cutout", "cutout", {"all": texture_paths["binary"]}
                ),
                "transparent": generated_block(
                    "test:transparent",
                    "transparent",
                    {"all": texture_paths["fractional"]},
                ),
                "mixed": generated_block(
                    "test:mixed",
                    "opaque",
                    {
                        "frame": texture_paths["opaque"],
                        "pane": texture_paths["fractional"],
                    },
                    {"pane": "transparent"},
                ),
                "binary_blend": generated_block(
                    "test:binary_blend",
                    "transparent",
                    {"pane": texture_paths["binary"]},
                ),
                "empty": generated_block("test:empty", "opaque"),
            }
            for name, payload in fixtures.items():
                rigel_assets.write_output(
                    output, f"blocks/{name}.yaml", payload
                )

            report = rigel_assets.audit_generated_material_layers(output)

            self.assertEqual(
                report["texture_alpha_classes"],
                {"fully_opaque": 1, "binary": 1, "fractional": 1},
            )
            self.assertEqual(
                report["effective_registration_layers"],
                {
                    "opaque_only": 1,
                    "cutout_only": 1,
                    "transparent_only": 2,
                    "mixed": 1,
                    "other_only": 0,
                    "empty": 1,
                },
            )
            self.assertEqual(
                report["suspicious_cross_classifications"],
                [{
                    "identifier": "test:binary_blend",
                    "slot": "pane",
                    "texture": texture_paths["binary"],
                    "alpha_class": "binary",
                    "render_layer": "transparent",
                }],
            )
            rigel_assets.validate_material_layer_audit_report(
                report, {"textures": 3, "blocks": 6}
            )

    def test_rejects_incomplete_or_ambiguous_block_model_closure(self) -> None:
        def duplicate_model(output: Path) -> None:
            shutil.copyfile(
                output / "models/blocks/post.yaml",
                output / "models/blocks/duplicate.yaml",
            )

        def malformed_primitive(output: Path) -> None:
            path = output / "models/blocks/post.yaml"
            model = json.loads(path.read_bytes())
            model["cuboids"][0]["faces"] = {}
            path.write_bytes(encoded_json(model))

        def unresolved_model(output: Path) -> None:
            path = output / "blocks/test__post.yaml"
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "base:block_model/post", "test:missing"
                ),
                encoding="utf-8",
            )

        def unresolved_slot(output: Path) -> None:
            path = output / "blocks/test__post.yaml"
            lines = path.read_text(encoding="utf-8").splitlines()
            path.write_text(
                "\n".join(line for line in lines if not line.startswith("  accent:"))
                + "\n",
                encoding="utf-8",
            )

        def missing_texture(output: Path) -> None:
            (output / "textures/blocks/red_surface.png").unlink()

        def invalid_orientation(output: Path) -> None:
            path = output / "blocks/test__post.yaml"
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "layer:", "orientation: [90, 90, 0]\nlayer:"
                ),
                encoding="utf-8",
            )

        def namespace_collision(output: Path) -> None:
            model_path = output / "models/blocks/post.yaml"
            model = json.loads(model_path.read_bytes())
            model["id"] = "test:post"
            model_path.write_bytes(encoded_json(model))
            for block_path in (output / "blocks").glob("*.yaml"):
                block_path.write_text(
                    block_path.read_text(encoding="utf-8").replace(
                        "base:block_model/post", "test:post"
                    ),
                    encoding="utf-8",
                )

        def duplicate_entity_model_id(output: Path) -> None:
            rigel_assets.write_output(
                output,
                "models/entities/first.json",
                encoded_json({"id": "shared", "textures": {}}),
            )
            rigel_assets.write_output(
                output,
                "models/entities/second.json",
                encoded_json({"id": "shared", "textures": {}}),
            )

        def nontexture_resource(output: Path) -> None:
            block_path = output / "blocks/test__post.yaml"
            block_path.write_text(
                block_path.read_text(encoding="utf-8").replace(
                    "textures/blocks/red_surface.png", "sounds/not-a-texture.ogg"
                ),
                encoding="utf-8",
            )
            rigel_assets.write_output(output, "sounds/not-a-texture.ogg", b"sound")

        cases = (
            ("duplicate normalized block model identifier", duplicate_model),
            ("at least one visible face", malformed_primitive),
            ("unresolved normalized block model", unresolved_model),
            ("texture bindings do not match model", unresolved_slot),
            ("texture references are unresolved", missing_texture),
            ("supported block-state set", invalid_orientation),
            ("block/model identifier namespace collision", namespace_collision),
            ("duplicate entity model identifier", duplicate_entity_model_id),
            ("invalid generated texture reference", nontexture_resource),
        )
        for diagnostic, mutate in cases:
            with self.subTest(diagnostic=diagnostic), tempfile.TemporaryDirectory() as directory:
                output = compile_cuboid_fixture(Path(directory))
                mutate(output)
                with self.assertRaisesRegex(
                    rigel_assets.AssetImportError, diagnostic
                ):
                    rigel_assets.validate_generated_tree(
                        output, required_identifiers=("test:post",)
                    )


class SynchronizationTest(unittest.TestCase):
    def test_sync_omits_ambiguous_planes_from_collision_publication(self) -> None:
        entries = synthetic_block_entries()
        entries["base/models/blocks/plane.json"] = encoded_json(
            {
                "textures": {
                    "all": {
                        "fileName": "base:textures/blocks/test_stone.png"
                    }
                },
                "planes": [{"invented": "unsupported"}],
            }
        )
        entries["base/blocks/plane.json"] = encoded_json(
            {
                "stringId": "test:plane",
                "defaultProperties": {
                    "modelName": "base:models/blocks/plane.json"
                },
                "blockStates": {"default": {}},
            }
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)

            provenance, changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )

            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            self.assertTrue(changed)
            self.assertFalse((assets / "blocks/test__plane.yaml").exists())
            self.assertEqual(
                provenance["block_collision_import"],
                {
                    "schema": rigel_assets.BLOCK_COLLISION_SUPPORT_SCHEMA,
                    "empty": 2,
                    "full": 2,
                    "single_partial": 0,
                    "multi_box": 0,
                    "exact_derived": 4,
                    "conservative_fallback": 0,
                    "ambiguous": 0,
                },
            )
            self.assertEqual(
                provenance["block_model_import"]["omissions"]
                ["plane_or_mixed_geometry"]["block_states"],
                ["test:plane"],
            )
            self.assertTrue(
                any(
                    "omitted 1 block states with explicit plane geometry"
                    in diagnostic
                    for diagnostic in provenance["source_omissions"]
                )
            )

    def test_sync_records_block_model_recovery_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_cuboid_entries())

            provenance, changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:post",)
            )

            self.assertTrue(changed)
            self.assertEqual(provenance["counts"]["block_models"], 1)
            self.assertEqual(
                provenance["block_model_import"],
                {
                    "schema": rigel_assets.BLOCK_MODEL_SUPPORT_SCHEMA,
                    "candidate_states": 1,
                    "newly_recovered_states": 1,
                    "base_approximation_states": 2,
                    "corrected_approximations": 2,
                    "omissions": {
                        "plane_or_mixed_geometry": {
                            "block_states": [],
                            "candidate_states": 0,
                            "base_approximation_states": 0,
                            "other_states": 0,
                        },
                        "nonstandard_texture_dimensions": {
                            "block_states": [],
                            "candidate_states": 0,
                            "base_approximation_states": 0,
                            "other_states": 0,
                        },
                    },
                },
            )

    def test_block_model_omission_reasons_are_disjoint_and_precise(self) -> None:
        cases: list[tuple[str, dict[str, bytes], str]] = []
        plane_entries = synthetic_cuboid_entries()
        plane_model = json.loads(plane_entries["base/models/blocks/post.json"])
        plane_model["planes"] = [{"invented": "unsupported"}]
        plane_entries["base/models/blocks/post.json"] = encoded_json(plane_model)
        cases.append(("plane_or_mixed_geometry", plane_entries, "explicit plane"))

        texture_entries = synthetic_cuboid_entries()
        for path in tuple(texture_entries):
            if path.startswith("base/textures/blocks/"):
                texture_entries[path] = synthetic_png(64, 16)
        cases.append(
            (
                "nonstandard_texture_dimensions",
                texture_entries,
                "not 16x16",
            )
        )

        for reason, entries, diagnostic in cases:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                jar = root / "fixture.jar"
                write_jar(jar, entries)
                output = root / "output"
                messages: list[str] = []
                audit = rigel_assets.BlockModelImportAudit()
                with zipfile.ZipFile(jar) as archive:
                    indexed = rigel_assets.indexed_archive(archive)
                    rigel_assets.extract_direct_assets(archive, indexed, output)
                    rigel_assets.compile_blocks(
                        archive, indexed, output, messages, audit
                    )
                rigel_assets.omit_blocks_with_unsupported_textures(
                    output, messages, audit
                )

                report = audit.provenance()
                self.assertEqual(report["candidate_states"], 1)
                self.assertEqual(report["newly_recovered_states"], 0)
                self.assertEqual(report["base_approximation_states"], 2)
                self.assertEqual(report["corrected_approximations"], 0)
                self.assertEqual(
                    report["omissions"][reason]["block_states"],
                    [
                        "test:post",
                        "test:post[color=blue]",
                        "test:post[shape=generated]",
                    ],
                )
                other_reason = (
                    "nonstandard_texture_dimensions"
                    if reason == "plane_or_mixed_geometry"
                    else "plane_or_mixed_geometry"
                )
                self.assertEqual(
                    report["omissions"][other_reason]["block_states"], []
                )
                self.assertTrue(any(diagnostic in message for message in messages))

    def test_sync_is_idempotent_and_records_provenance(self) -> None:
        entries = synthetic_block_entries()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, entries)

            first, first_changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            second, second_changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )

            self.assertTrue(first_changed)
            self.assertFalse(second_changed)
            self.assertEqual(first, second)
            self.assertEqual(first["schema"], 1)
            self.assertEqual(first["importer_schema"], rigel_assets.IMPORTER_SCHEMA)
            self.assertEqual(first["counts"]["blocks"], 4)
            self.assertEqual(
                first["output_tree_sha256"],
                rigel_assets.sha256_tree(root / ".rigel/assets"),
            )

    def test_block_model_support_schema_invalidates_synchronization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            first, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            updated_schema = rigel_assets.BLOCK_MODEL_SUPPORT_SCHEMA + 1

            with mock.patch.object(
                rigel_assets, "BLOCK_MODEL_SUPPORT_SCHEMA", updated_schema
            ):
                self.assertFalse(
                    rigel_assets.current_import_matches(
                        root, str(first["jar_sha256"])
                    )
                )
                rebuilt, changed = rigel_assets.synchronize(
                    root, jar, required_identifiers=("test:stone",)
                )

            self.assertTrue(changed)
            self.assertEqual(
                rebuilt["block_model_import"]["schema"], updated_schema
            )

    def test_block_collision_support_schema_invalidates_synchronization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            first, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            updated_schema = rigel_assets.BLOCK_COLLISION_SUPPORT_SCHEMA + 1

            with mock.patch.object(
                rigel_assets,
                "BLOCK_COLLISION_SUPPORT_SCHEMA",
                updated_schema,
            ):
                self.assertFalse(
                    rigel_assets.current_import_matches(
                        root, str(first["jar_sha256"])
                    )
                )
                rebuilt, changed = rigel_assets.synchronize(
                    root, jar, required_identifiers=("test:stone",)
                )

            self.assertTrue(changed)
            self.assertEqual(
                rebuilt["block_collision_import"]["schema"], updated_schema
            )

    def test_existing_import_rejects_inconsistent_model_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_cuboid_entries())
            provenance, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:post",)
            )
            provenance["block_model_import"]["corrected_approximations"] = 1
            rigel_assets.atomic_write_json(
                root / rigel_assets.PROVENANCE_RELATIVE_PATH, provenance
            )

            with self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "approximation correction count is inconsistent",
            ):
                rigel_assets.validate_existing_import(root)
            self.assertFalse(
                rigel_assets.current_import_matches(
                    root, str(provenance["jar_sha256"])
                )
            )

    def test_existing_import_rejects_inconsistent_collision_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            provenance, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            provenance["block_collision_import"]["full"] += 1
            rigel_assets.atomic_write_json(
                root / rigel_assets.PROVENANCE_RELATIVE_PATH, provenance
            )

            with self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "must be exact derivations",
            ):
                rigel_assets.validate_existing_import(root)
            self.assertFalse(
                rigel_assets.current_import_matches(
                    root, str(provenance["jar_sha256"])
                )
            )

    def test_sync_repairs_plausible_false_collision_fallback_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            provenance, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            report = provenance["block_collision_import"]
            expected_report = dict(report)
            report["exact_derived"] -= 1
            report["conservative_fallback"] = 1
            report["ambiguous"] = 1
            shape_counts = rigel_assets.audit_generated_collision_shapes(
                root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            )

            with self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "must be exact derivations",
            ):
                rigel_assets.validate_block_collision_import_report(
                    report, shape_counts
                )

            rigel_assets.atomic_write_json(
                root / rigel_assets.PROVENANCE_RELATIVE_PATH, provenance
            )
            with self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "must be exact derivations",
            ):
                rigel_assets.validate_existing_import(root)
            self.assertFalse(
                rigel_assets.current_import_matches(
                    root, str(provenance["jar_sha256"])
                )
            )

            repaired, changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )

            self.assertTrue(changed)
            self.assertEqual(
                repaired["block_collision_import"], expected_report
            )
            self.assertTrue(
                rigel_assets.current_import_matches(
                    root, str(repaired["jar_sha256"])
                )
            )

    def test_existing_import_rejects_stale_material_layer_audit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            provenance, _ = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )
            layer_counts = provenance["material_layer_audit"][
                "effective_registration_layers"
            ]
            layer_counts["opaque_only"] -= 1
            layer_counts["transparent_only"] += 1
            rigel_assets.atomic_write_json(
                root / rigel_assets.PROVENANCE_RELATIVE_PATH, provenance
            )

            validate_tree = rigel_assets.validate_generated_tree
            with mock.patch.object(
                rigel_assets,
                "validate_generated_tree",
                side_effect=lambda assets: validate_tree(
                    assets, required_identifiers=("test:stone",)
                ),
            ), self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "material-layer audit does not match",
            ):
                rigel_assets.validate_existing_import(root)

    def test_forced_rebuild_is_byte_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_cuboid_entries())

            first, first_changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:post",)
            )
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            first_files = {
                path.relative_to(assets): path.read_bytes()
                for path in assets.rglob("*")
                if path.is_file()
            }
            first_provenance = provenance_path.read_bytes()

            rebuilt, rebuilt_changed = rigel_assets.synchronize(
                root,
                jar,
                force=True,
                required_identifiers=("test:post",),
            )

            self.assertTrue(first_changed)
            self.assertTrue(rebuilt_changed)
            self.assertEqual(rebuilt, first)
            self.assertEqual(provenance_path.read_bytes(), first_provenance)
            self.assertEqual(
                {
                    path.relative_to(assets): path.read_bytes()
                    for path in assets.rglob("*")
                    if path.is_file()
                },
                first_files,
            )
            self.assertEqual(
                rigel_assets.sha256_tree(assets), first["output_tree_sha256"]
            )

    def test_sync_imports_the_same_jar_bytes_recorded_in_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_jar = root / "first.jar"
            replacement_jar = root / "replacement.jar"
            first_entries = synthetic_block_entries()
            first_entries["base/version.txt"] = b"first"
            first_entries["base/sounds/identity.ogg"] = b"first"
            replacement_entries = synthetic_block_entries()
            replacement_entries["base/version.txt"] = b"replacement"
            replacement_entries["base/sounds/identity.ogg"] = b"replacement"
            write_jar(first_jar, first_entries)
            write_jar(replacement_jar, replacement_entries)
            staged = rigel_assets.stage_jar(root, first_jar)
            first_digest = rigel_assets.sha256_file(first_jar)
            replacement_digest = rigel_assets.sha256_file(replacement_jar)
            real_synchronization_lock = rigel_assets._synchronization_lock
            replacement_count = 0

            @contextlib.contextmanager
            def replace_before_archive_open(lock_root: Path, jar_digest: str):
                nonlocal replacement_count
                self.assertEqual(jar_digest, first_digest)
                replacement_count += 1
                rigel_assets.stage_jar(root, replacement_jar)
                with real_synchronization_lock(lock_root, jar_digest):
                    yield

            with mock.patch.object(
                rigel_assets,
                "_synchronization_lock",
                side_effect=replace_before_archive_open,
            ):
                provenance, changed = rigel_assets.synchronize(
                    root, required_identifiers=("test:stone",)
                )

            self.assertTrue(changed)
            self.assertEqual(replacement_count, 1)
            self.assertEqual(provenance["jar_sha256"], first_digest)
            self.assertEqual(provenance.get("source_version"), "first")
            self.assertEqual(
                (root / ".rigel/assets/sounds/identity.ogg").read_bytes(),
                b"first",
            )
            self.assertEqual(rigel_assets.sha256_file(staged), replacement_digest)
            self.assertTrue(
                rigel_assets.current_import_matches(root, first_digest)
            )
            self.assertFalse(
                rigel_assets.current_import_matches(root, replacement_digest)
            )

    def test_equivalent_concurrent_syncs_share_completed_import(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, synthetic_block_entries())
            first_started = threading.Event()
            continue_first = threading.Event()
            second_started = threading.Event()
            results: list[bool] = []
            errors: list[BaseException] = []
            extraction_count = 0
            real_extract = rigel_assets.extract_direct_assets

            def pause_first_extract(*args: object, **kwargs: object) -> object:
                nonlocal extraction_count
                extraction_count += 1
                if extraction_count == 1:
                    first_started.set()
                    if not continue_first.wait(timeout=5):
                        raise AssertionError("equivalent synchronization did not start")
                return real_extract(*args, **kwargs)

            def synchronize() -> None:
                try:
                    _, changed = rigel_assets.synchronize(
                        root, jar, required_identifiers=("test:stone",)
                    )
                    results.append(changed)
                except BaseException as error:
                    errors.append(error)

            def synchronize_second() -> None:
                second_started.set()
                synchronize()

            with mock.patch.object(
                rigel_assets,
                "extract_direct_assets",
                side_effect=pause_first_extract,
            ):
                first = threading.Thread(target=synchronize)
                second = threading.Thread(target=synchronize_second)
                first.start()
                self.assertTrue(first_started.wait(timeout=5))
                second.start()
                self.assertTrue(second_started.wait(timeout=5))
                continue_first.set()
                first.join(timeout=5)
                second.join(timeout=5)

            self.assertFalse(first.is_alive())
            self.assertFalse(second.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(sorted(results), [False, True])
            self.assertEqual(extraction_count, 1)

    def test_older_concurrent_sync_cannot_replace_newer_publication(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            older_jar = root / "older.jar"
            newer_jar = root / "newer.jar"
            older_entries = synthetic_block_entries()
            older_entries["base/version.txt"] = b"older"
            newer_entries = synthetic_block_entries()
            newer_entries["base/version.txt"] = b"newer"
            write_jar(older_jar, older_entries)
            write_jar(newer_jar, newer_entries)
            older_ready = threading.Event()
            continue_older = threading.Event()
            older_errors: list[BaseException] = []
            real_validate = rigel_assets.validate_generated_tree

            def pause_older_validation(*args: object, **kwargs: object) -> object:
                if threading.current_thread().name == "older-synchronization":
                    older_ready.set()
                    if not continue_older.wait(timeout=5):
                        raise AssertionError("newer synchronization did not complete")
                return real_validate(*args, **kwargs)

            def synchronize_older() -> None:
                try:
                    rigel_assets.synchronize(
                        root,
                        older_jar,
                        required_identifiers=("test:stone",),
                    )
                except BaseException as error:
                    older_errors.append(error)

            with mock.patch.object(
                rigel_assets,
                "validate_generated_tree",
                side_effect=pause_older_validation,
            ):
                older = threading.Thread(
                    target=synchronize_older,
                    name="older-synchronization",
                )
                older.start()
                self.assertTrue(older_ready.wait(timeout=5))
                newer, changed = rigel_assets.synchronize(
                    root,
                    newer_jar,
                    required_identifiers=("test:stone",),
                )
                continue_older.set()
                older.join(timeout=5)

            self.assertFalse(older.is_alive())
            self.assertTrue(changed)
            self.assertEqual(newer.get("source_version"), "newer")
            self.assertEqual(len(older_errors), 1)
            self.assertIsInstance(older_errors[0], rigel_assets.AssetImportError)
            self.assertIn("changed while synchronization", str(older_errors[0]))
            published = rigel_assets.read_provenance(root)
            self.assertIsNotNone(published)
            assert published is not None
            self.assertEqual(published.get("source_version"), "newer")
            self.assertTrue(
                rigel_assets.current_import_matches(
                    root, rigel_assets.sha256_file(newer_jar)
                )
            )

    def test_failed_repair_of_tampered_publication_can_be_retried(self) -> None:
        for phase in ("previous-tree move", "staging install", "provenance write"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                first_jar = root / "first.jar"
                replacement_jar = root / "replacement.jar"
                write_jar(first_jar, synthetic_block_entries())
                replacement_entries = synthetic_block_entries()
                replacement_entries["base/version.txt"] = b"replacement"
                write_jar(replacement_jar, replacement_entries)
                rigel_assets.synchronize(
                    root, first_jar, required_identifiers=("test:stone",)
                )
                assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
                provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
                (assets / "textures/blocks/test_stone.png").write_bytes(b"tampered")
                tampered_hash = rigel_assets.sha256_tree(assets)
                previous_provenance = provenance_path.read_bytes()

                if phase == "provenance write":
                    real_atomic_write = rigel_assets.atomic_write_json

                    def fail_publication_provenance(
                        destination: Path, value: dict[str, object]
                    ) -> None:
                        if destination == provenance_path:
                            raise OSError("injected provenance write failure")
                        real_atomic_write(destination, value)

                    failure = mock.patch.object(
                        rigel_assets,
                        "atomic_write_json",
                        side_effect=fail_publication_provenance,
                    )
                else:
                    real_replace = os.replace

                    def fail_publication_move(
                        source: object, destination: object
                    ) -> None:
                        source_path = Path(source)
                        destination_path = Path(destination)
                        moving_previous = (
                            source_path == assets
                            and destination_path.name == "assets"
                            and destination_path.parent.name.startswith(
                                ".assets-previous-"
                            )
                        )
                        installing_staging = (
                            source_path.name.startswith(
                                rigel_assets.STAGING_PREFIX
                            )
                            and destination_path == assets
                        )
                        if (
                            phase == "previous-tree move" and moving_previous
                        ) or (phase == "staging install" and installing_staging):
                            raise OSError(f"injected {phase} failure")
                        real_replace(source, destination)

                    failure = mock.patch.object(
                        rigel_assets.os,
                        "replace",
                        side_effect=fail_publication_move,
                    )

                with failure, self.assertRaisesRegex(OSError, phase):
                    rigel_assets.synchronize(
                        root,
                        replacement_jar,
                        required_identifiers=("test:stone",),
                    )

                self.assertEqual(rigel_assets.sha256_tree(assets), tampered_hash)
                self.assertEqual(provenance_path.read_bytes(), previous_provenance)
                self.assertFalse(any((root / ".rigel").glob(".assets-previous-*")))

                replacement, changed = rigel_assets.synchronize(
                    root,
                    replacement_jar,
                    required_identifiers=("test:stone",),
                )
                self.assertTrue(changed)
                self.assertEqual(replacement.get("source_version"), "replacement")
                self.assertTrue(
                    rigel_assets.current_import_matches(
                        root, rigel_assets.sha256_file(replacement_jar)
                    )
                )

    def test_source_change_replaces_tree_instead_of_accumulating(self) -> None:
        first_entries = synthetic_cuboid_entries()
        first_entries["base/sounds/stale.ogg"] = b"old"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, first_entries)
            rigel_assets.synchronize(root, jar, required_identifiers=("test:post",))
            stale = root / ".rigel/assets/sounds/stale.ogg"
            stale_model = root / ".rigel/assets/models/blocks/post.yaml"
            stale_variant = (
                root
                / ".rigel/assets/blocks/test__post[shape=generated].yaml"
            )
            self.assertTrue(stale.is_file())
            self.assertTrue(stale_model.is_file())
            self.assertTrue(stale_variant.is_file())

            second_entries = synthetic_block_entries()
            second_entries["base/version.txt"] = b"second"
            write_jar(jar, second_entries)
            _, changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )

            self.assertTrue(changed)
            self.assertFalse(stale.exists())
            self.assertFalse(stale_model.exists())
            self.assertFalse(stale_variant.exists())
            self.assertFalse(any((root / ".rigel/assets/blocks").glob("*post*")))

    def test_failed_sync_preserves_previous_valid_tree_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good_jar = root / "good.jar"
            write_jar(good_jar, synthetic_block_entries())
            rigel_assets.synchronize(
                root, good_jar, required_identifiers=("test:stone",)
            )
            previous_hash = rigel_assets.sha256_tree(root / ".rigel/assets")
            previous_provenance = (
                root / rigel_assets.PROVENANCE_RELATIVE_PATH
            ).read_bytes()

            bad_entries = synthetic_block_entries()
            block = json.loads(bad_entries["base/blocks/test_stone.json"])
            block["blockStates"]["default"]["futurePhysics"] = True
            bad_entries["base/blocks/test_stone.json"] = encoded_json(block)
            bad_jar = root / "bad.jar"
            write_jar(bad_jar, bad_entries)
            with self.assertRaisesRegex(rigel_assets.AssetImportError, "futurePhysics"):
                rigel_assets.synchronize(
                    root, bad_jar, required_identifiers=("test:stone",)
                )

            self.assertEqual(
                previous_hash, rigel_assets.sha256_tree(root / ".rigel/assets")
            )
            self.assertEqual(
                previous_provenance,
                (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_bytes(),
            )

    def test_png_codec_failure_preserves_previous_tree_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good_jar = root / "good.jar"
            write_jar(good_jar, synthetic_block_entries())
            previous, _ = rigel_assets.synchronize(
                root, good_jar, required_identifiers=("test:stone",)
            )
            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            provenance_path = root / rigel_assets.PROVENANCE_RELATIVE_PATH
            previous_hash = rigel_assets.sha256_tree(assets)
            previous_provenance = provenance_path.read_bytes()

            bad_entries = synthetic_block_entries()
            bad_entries["base/textures/blocks/test_stone.png"] = (
                png_with_lowercase_reserved_chunk()
            )
            bad_jar = root / "bad.jar"
            write_jar(bad_jar, bad_entries)
            with self.assertRaisesRegex(
                rigel_assets.AssetImportError, "lowercase reserved"
            ):
                rigel_assets.synchronize(
                    root, bad_jar, required_identifiers=("test:stone",)
                )

            self.assertEqual(rigel_assets.sha256_tree(assets), previous_hash)
            self.assertEqual(provenance_path.read_bytes(), previous_provenance)
            self.assertTrue(
                rigel_assets.current_import_matches(
                    root, str(previous["jar_sha256"])
                )
            )

    def test_validation_rejects_missing_texture_reference(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            block = rigel_assets.render_block_yaml(
                "test:stone",
                {"modelName": "unused"},
                _MissingTextureModelResolver(),
                "fixture",
            )
            rigel_assets.write_output(root, "blocks/test__stone.yaml", block)
            with self.assertRaisesRegex(rigel_assets.AssetImportError, "unresolved"):
                rigel_assets.validate_generated_tree(
                    root, required_identifiers=("test:stone",)
                )

    def test_validation_rejects_unknown_and_incoherent_layers(self) -> None:
        base = rigel_assets.render_block_yaml(
            "test:stone",
            {"modelName": "unused"},
            _MissingTextureModelResolver(),
            "fixture",
        )
        cases = (
            (b"layer: opaque", b"layer: unknown", "invalid render layer"),
            (
                b"layer: opaque",
                b"layer: transparent",
                "fully_opaque PNG alpha",
            ),
        )
        for original, replacement, message in cases:
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                rigel_assets.write_output(
                    root,
                    "blocks/test__stone.yaml",
                    base.replace(original, replacement),
                )
                rigel_assets.write_output(
                    root, "textures/blocks/missing.png", synthetic_png()
                )
                with self.assertRaisesRegex(
                    rigel_assets.AssetImportError, message
                ):
                    rigel_assets.validate_generated_tree(
                        root, required_identifiers=("test:stone",)
                    )

    def test_validation_rejects_malformed_and_noncanonical_collision(self) -> None:
        base = rigel_assets.render_block_yaml(
            "test:stone",
            {"modelName": "unused"},
            _MissingTextureModelResolver(),
            "fixture",
        )
        cases = (
            (
                b"collision: full\n",
                b"",
                "missing generated fields: collision",
            ),
            (
                b"collision_provenance: exact\n",
                b"",
                "missing generated fields: collision_provenance",
            ),
            (
                b"collision_provenance: exact",
                b"collision_provenance: guessed",
                "unsupported collision provenance",
            ),
            (
                b"collision: full",
                b'collision: {"boxes":[]}',
                "non-empty array",
            ),
            (
                b"collision: full",
                b'collision: {"boxes":[[0,0,0,1,1,1]]}',
                "canonical unit geometry",
            ),
            (
                b"collision: full",
                b'collision: {"boxes":[[-0.26,0,0,1,1,1]]}',
                "supported.*range",
            ),
        )
        for original, replacement, message in cases:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                rigel_assets.write_output(
                    root,
                    "blocks/test__stone.yaml",
                    base.replace(original, replacement),
                )
                rigel_assets.write_output(
                    root, "textures/blocks/missing.png", synthetic_png()
                )
                with self.assertRaisesRegex(
                    rigel_assets.AssetImportError, message
                ):
                    rigel_assets.validate_generated_tree(
                        root, required_identifiers=("test:stone",)
                    )

    def test_validation_enforces_collision_box_cardinality(self) -> None:
        base = rigel_assets.render_block_yaml(
            "test:stone",
            {"modelName": "unused"},
            _MissingTextureModelResolver(),
            "fixture",
        )

        def boxes(count: int) -> list[list[float]]:
            return [
                [-0.25 + index * 0.01, 0.0, 0.0, 1.25, 1.0, 1.0]
                for index in range(count)
            ]

        def generated_block(count: int) -> bytes:
            replacement = json.dumps(
                {"boxes": boxes(count)}, separators=(",", ":")
            ).encode("utf-8")
            return base.replace(b"collision: full", b"collision: " + replacement)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rigel_assets.write_output(
                root,
                "blocks/test__stone.yaml",
                generated_block(rigel_assets.BLOCK_COLLISION_MAXIMUM_BOXES),
            )
            rigel_assets.write_output(
                root, "textures/blocks/missing.png", synthetic_png()
            )
            counts = rigel_assets.validate_generated_tree(
                root, required_identifiers=("test:stone",)
            )
            self.assertEqual(counts["blocks"], 1)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rigel_assets.write_output(
                root,
                "blocks/test__stone.yaml",
                generated_block(
                    rigel_assets.BLOCK_COLLISION_MAXIMUM_BOXES + 1
                ),
            )
            rigel_assets.write_output(
                root, "textures/blocks/missing.png", synthetic_png()
            )
            with self.assertRaisesRegex(
                rigel_assets.AssetImportError,
                "contains 17 boxes; at most 16",
            ):
                rigel_assets.validate_generated_tree(
                    root, required_identifiers=("test:stone",)
                )


class RealJarBlockModelClosureTest(unittest.TestCase):
    def test_cosmic_reach_0_6_1_model_closure(self) -> None:
        configured_jar = os.environ.get(rigel_assets.JAR_ENVIRONMENT_VARIABLE)
        if not configured_jar:
            self.skipTest(
                f"{rigel_assets.JAR_ENVIRONMENT_VARIABLE} does not select a real JAR"
            )
        jar = rigel_assets.validate_jar(Path(configured_jar))
        with zipfile.ZipFile(jar) as archive:
            version = rigel_assets.source_version(
                archive, rigel_assets.indexed_archive(archive)
            )
        if version != "0.6.1":
            self.skipTest(f"configured Cosmic Reach JAR is version {version!r}")

        self.assertEqual(
            rigel_assets.sha256_file(jar),
            "58a2cc3b79b5413cfa0f2e4ae3b37f44ed7f11a5e828de57f9f3f71599ac570e",
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, changed = rigel_assets.synchronize(root, jar)
            first_provenance = (
                root / rigel_assets.PROVENANCE_RELATIVE_PATH
            ).read_bytes()
            rebuilt, rebuilt_changed = rigel_assets.synchronize(
                root, jar, force=True
            )
            repeated, repeated_changed = rigel_assets.synchronize(root, jar)

            self.assertTrue(changed)
            self.assertTrue(rebuilt_changed)
            self.assertFalse(repeated_changed)
            self.assertEqual(rebuilt, first)
            self.assertEqual(repeated, first)
            self.assertEqual(
                (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_bytes(),
                first_provenance,
            )
            self.assertEqual(
                first["counts"],
                {
                    "blocks": 2021,
                    "textures": 438,
                    "models": 16,
                    "block_models": 51,
                    "animations": 7,
                    "sounds": 59,
                },
            )
            report = first["block_model_import"]
            self.assertEqual(report["candidate_states"], 1607)
            self.assertEqual(report["newly_recovered_states"], 1590)
            self.assertEqual(report["base_approximation_states"], 106)
            self.assertEqual(report["corrected_approximations"], 100)
            plane = report["omissions"]["plane_or_mixed_geometry"]
            self.assertEqual(len(plane["block_states"]), 9)
            self.assertEqual(plane["candidate_states"], 6)
            self.assertEqual(plane["base_approximation_states"], 3)
            textures = report["omissions"][
                "nonstandard_texture_dimensions"
            ]
            self.assertEqual(len(textures["block_states"]), 14)
            self.assertEqual(textures["candidate_states"], 11)
            self.assertEqual(textures["base_approximation_states"], 3)
            self.assertEqual(
                first["material_layer_audit"],
                {
                    "schema": rigel_assets.MATERIAL_LAYER_AUDIT_SCHEMA,
                    "texture_alpha_classes": {
                        "fully_opaque": 252,
                        "binary": 152,
                        "fractional": 34,
                    },
                    "effective_registration_layers": {
                        "opaque_only": 1709,
                        "cutout_only": 271,
                        "transparent_only": 26,
                        "mixed": 14,
                        "other_only": 0,
                        "empty": 1,
                    },
                    "suspicious_cross_classifications": [],
                },
            )
            self.assertEqual(
                first["block_collision_import"],
                {
                    "schema": rigel_assets.BLOCK_COLLISION_SUPPORT_SCHEMA,
                    "empty": 67,
                    "full": 315,
                    "single_partial": 956,
                    "multi_box": 683,
                    "exact_derived": 2021,
                    "conservative_fallback": 0,
                    "ambiguous": 0,
                },
            )

            assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
            generated_blocks = {}
            for path in sorted((assets / "blocks").glob("*.yaml")):
                block = rigel_assets.parse_generated_block(
                    path.read_bytes(), path.relative_to(assets).as_posix()
                )
                generated_blocks[str(block["id"])] = block

            explicit_box_cardinalities = {
                identifier: len(block["collision"]["boxes"])
                for identifier, block in generated_blocks.items()
                if isinstance(block["collision"], dict)
            }
            measured_maximum_boxes = max(explicit_box_cardinalities.values())
            self.assertEqual(measured_maximum_boxes, 7)
            self.assertLessEqual(
                measured_maximum_boxes,
                rigel_assets.BLOCK_COLLISION_MAXIMUM_BOXES,
            )
            self.assertEqual(
                {
                    identifier
                    for identifier, count in explicit_box_cardinalities.items()
                    if count == measured_maximum_boxes
                },
                {"base:laser_emitter[type=split,axis=Y]"},
            )

            self.assertEqual(
                rigel_assets.audit_generated_collision_shapes(assets),
                {
                    "empty": 67,
                    "full": 315,
                    "single_partial": 956,
                    "multi_box": 683,
                    "exact_derived": 2021,
                    "conservative_fallback": 0,
                    "ambiguous": 0,
                },
            )
            self.assertTrue(all(
                block["collision_provenance"] == "exact"
                for block in generated_blocks.values()
            ))

            walk_through_with_geometry = {
                identifier
                for identifier, block in generated_blocks.items()
                if not block["solid"] and block["model"] != "none"
            }
            self.assertEqual(len(walk_through_with_geometry), 66)
            self.assertTrue(all(
                generated_blocks[identifier]["collision"] == "none"
                for identifier in walk_through_with_geometry
            ))
            self.assertEqual(
                {
                    identifier
                    for identifier, block in generated_blocks.items()
                    if not block["solid"] and block["model"] == "none"
                },
                {"base:air"},
            )

            slab_boxes = {
                "bottom": [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                "top": [0.0, 0.5, 0.0, 1.0, 1.0, 1.0],
                "verticalNegX": [0.0, 0.0, 0.0, 0.5, 1.0, 1.0],
                "verticalNegZ": [0.0, 0.0, 0.0, 1.0, 1.0, 0.5],
                "verticalPosX": [0.5, 0.0, 0.0, 1.0, 1.0, 1.0],
                "verticalPosZ": [0.0, 0.0, 0.5, 1.0, 1.0, 1.0],
            }
            for slab_type, box in slab_boxes.items():
                with self.subTest(slab_type=slab_type):
                    self.assertEqual(
                        generated_blocks[
                            f"base:wood_planks[slab_type={slab_type}]"
                        ]["collision"],
                        {"boxes": [box]},
                    )

            stair_boxes = {
                "bottom_NegX": [
                    [0.0, 0.5, 0.0, 0.5, 1.0, 1.0],
                    [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                ],
                "bottom_NegZ": [
                    [0.0, 0.5, 0.0, 1.0, 1.0, 0.5],
                    [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                ],
                "bottom_PosX": [
                    [0.5, 0.5, 0.0, 1.0, 1.0, 1.0],
                    [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                ],
                "bottom_PosZ": [
                    [0.0, 0.5, 0.5, 1.0, 1.0, 1.0],
                    [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                ],
                "top_NegX": [
                    [0.0, 0.0, 0.0, 0.5, 0.5, 1.0],
                    [0.0, 0.5, 0.0, 1.0, 1.0, 1.0],
                ],
                "top_NegZ": [
                    [0.0, 0.0, 0.0, 1.0, 0.5, 0.5],
                    [0.0, 0.5, 0.0, 1.0, 1.0, 1.0],
                ],
                "top_PosX": [
                    [0.5, 0.0, 0.0, 1.0, 0.5, 1.0],
                    [0.0, 0.5, 0.0, 1.0, 1.0, 1.0],
                ],
                "top_PosZ": [
                    [0.0, 0.0, 0.5, 1.0, 0.5, 1.0],
                    [0.0, 0.5, 0.0, 1.0, 1.0, 1.0],
                ],
            }
            for stair_type, boxes in stair_boxes.items():
                with self.subTest(stair_type=stair_type):
                    self.assertEqual(
                        generated_blocks[
                            f"base:wood_planks[stair_type={stair_type}]"
                        ]["collision"],
                        {"boxes": boxes},
                    )

            representative_collisions = {
                "base:steel_walkway": "full",
                "base:glass": "full",
                "base:ladder_steel[direction=PosX]": {
                    "boxes": [[0.99375, 0.0, 0.0, 1.0, 1.0, 1.0]]
                },
                "base:steel_handrail[direction=PosX]": {
                    "boxes": [[0.99375, 0.0, 0.0, 1.0, 1.0, 1.0]]
                },
                "base:grass_blades": "none",
                "base:table_pedestal_wood": {
                    "boxes": [
                        [0.0, 0.9375, 0.0, 0.0625, 1.0, 1.0],
                        [0.0625, 0.9375, 0.0, 0.9375, 1.0, 0.0625],
                        [0.0625, 0.9375, 0.0625, 0.9375, 1.0, 0.9375],
                        [0.0625, 0.9375, 0.9375, 0.9375, 1.0, 1.0],
                        [0.9375, 0.9375, 0.0, 1.0, 1.0, 1.0],
                        [0.4375, 0.0, 0.4375, 0.5625, 0.9375, 0.5625],
                    ]
                },
                "base:laser_emitter[type=single,direction=NegZ]": {
                    "boxes": [
                        [0.0, 0.0, 0.0, 1.0, 0.4375, 1.0],
                        [0.0625, 0.4375, 0.0625, 0.9375, 0.5625, 0.9375],
                        [0.0, 0.5625, 0.0, 1.0, 1.0, 1.0],
                    ]
                },
            }
            for identifier, collision in representative_collisions.items():
                with self.subTest(identifier=identifier):
                    self.assertEqual(
                        generated_blocks[identifier]["collision"], collision
                    )

            piston_boxes = {
                "NegX": [
                    [0.0, 0.0, 0.0, 0.25, 1.0, 1.0],
                    [0.25, 0.375, 0.375, 1.25, 0.625, 0.625],
                ],
                "NegY": [
                    [0.0, 0.0, 0.0, 1.0, 0.25, 1.0],
                    [0.375, 0.25, 0.375, 0.625, 1.25, 0.625],
                ],
                "NegZ": [
                    [0.0, 0.0, 0.0, 1.0, 1.0, 0.25],
                    [0.375, 0.375, 0.25, 0.625, 0.625, 1.25],
                ],
                "PosX": [
                    [0.75, 0.0, 0.0, 1.0, 1.0, 1.0],
                    [-0.25, 0.375, 0.375, 0.75, 0.625, 0.625],
                ],
                "PosY": [
                    [0.0, 0.75, 0.0, 1.0, 1.0, 1.0],
                    [0.375, -0.25, 0.375, 0.625, 0.75, 0.625],
                ],
                "PosZ": [
                    [0.0, 0.0, 0.75, 1.0, 1.0, 1.0],
                    [0.375, 0.375, -0.25, 0.625, 0.625, 0.75],
                ],
            }
            piston_types = ("advancing", "heavy", "push", "suction")
            expected_piston_heads = {
                f"base:piston[direction={direction},type={piston_type},part=head]"
                for direction in piston_boxes
                for piston_type in piston_types
            }
            self.assertEqual(
                {
                    identifier
                    for identifier in generated_blocks
                    if identifier.startswith("base:piston[direction=")
                    and identifier.endswith(",part=head]")
                },
                expected_piston_heads,
            )
            for direction, boxes in piston_boxes.items():
                for piston_type in piston_types:
                    identifier = (
                        f"base:piston[direction={direction},"
                        f"type={piston_type},part=head]"
                    )
                    with self.subTest(identifier=identifier):
                        self.assertTrue(generated_blocks[identifier]["solid"])
                        self.assertEqual(
                            generated_blocks[identifier]["collision"],
                            {"boxes": boxes},
                        )

            with zipfile.ZipFile(jar) as archive:
                entries = rigel_assets.indexed_archive(archive)
                models = rigel_assets.BlockModelResolver(archive, entries)

                def source_document(path: str) -> dict[str, object]:
                    value = rigel_assets.load_relaxed_json(
                        archive.read(entries[path]), path
                    )
                    self.assertIsInstance(value, dict)
                    return value

                grass_source = source_document(
                    "base/blocks/foliage/grass_blades.json"
                )
                grass_state = grass_source["blockStates"]["default"]
                self.assertIs(grass_state["walkThrough"], True)
                grass_model = models.resolve(str(grass_state["modelName"]))
                self.assertFalse(grass_model.empty)
                self.assertIsNotNone(grass_model.geometry)

                for model_name in (
                    "base:models/blocks/industrial_decor/steel_walkway.json",
                    "base:models/blocks/model_glass.json",
                ):
                    self.assertTrue(models.resolve(model_name).full_cube)

                for model_name in (
                    "base:models/blocks/furniture/model_ladder_steel.json",
                    "base:models/blocks/industrial_decor/steel_handrail.json",
                ):
                    geometry = models.resolve(model_name).geometry
                    self.assertIsNotNone(geometry)
                    self.assertEqual(
                        [cuboid.bounds for cuboid in geometry.cuboids],
                        [(0.0, 0.0, 0.0, 1.0, 1.0, 0.00625)],
                    )

                table_geometry = models.resolve(
                    "base:models/blocks/furniture/table_pedestal_wood.json"
                ).geometry
                self.assertIsNotNone(table_geometry)
                self.assertEqual(len(table_geometry.cuboids), 6)
                machine_geometry = models.resolve(
                    "base:models/blocks/machines/model_laser_emitter.json"
                ).geometry
                self.assertIsNotNone(machine_geometry)
                self.assertEqual(len(machine_geometry.cuboids), 3)
                piston_geometry = models.resolve(
                    "base:models/blocks/machines/pistons/model_piston_head.json"
                ).geometry
                self.assertIsNotNone(piston_geometry)
                self.assertEqual(
                    [cuboid.bounds for cuboid in piston_geometry.cuboids],
                    [
                        (0.0, 0.0, 0.0, 1.0, 1.0, 0.25),
                        (0.375, 0.375, 0.25, 0.625, 0.625, 1.25),
                    ],
                )

            representatives = {
                "base:leaves[type=permament]": (
                    "all", rigel_assets.TextureAlphaClass.BINARY, "cutout"
                ),
                "base:steel_walkway": (
                    "all", rigel_assets.TextureAlphaClass.BINARY, "cutout"
                ),
                "base:ladder_steel[direction=PosX]": (
                    "front", rigel_assets.TextureAlphaClass.BINARY, "cutout"
                ),
                "base:steel_handrail[direction=PosX]": (
                    "front", rigel_assets.TextureAlphaClass.BINARY, "cutout"
                ),
                "base:glass": (
                    "all", rigel_assets.TextureAlphaClass.FRACTIONAL,
                    "transparent",
                ),
                "base:water[type=source]": (
                    "all", rigel_assets.TextureAlphaClass.FRACTIONAL,
                    "transparent",
                ),
                "base:lava[type=source]": (
                    "all", rigel_assets.TextureAlphaClass.FULLY_OPAQUE,
                    "opaque",
                ),
                "base:grass_blades": (
                    "side", rigel_assets.TextureAlphaClass.BINARY, "cutout"
                ),
            }
            for identifier, (slot, expected_alpha, expected_layer) in (
                representatives.items()
            ):
                with self.subTest(identifier=identifier):
                    block = generated_blocks[identifier]
                    texture = str(block["textures"][slot])
                    self.assertEqual(
                        rigel_assets.classify_png_alpha(
                            (assets / texture).read_bytes(), texture
                        ),
                        expected_alpha,
                    )
                    self.assertEqual(
                        block.get("texture_render_layers", {}).get(
                            slot, block["layer"]
                        ),
                        expected_layer,
                    )
                    self.assertFalse(block["opaque"])

            table = generated_blocks["base:table_pedestal_wood"]
            self.assertEqual(table["layer"], "opaque")
            self.assertEqual(
                table["texture_render_layers"], {"top": "transparent"}
            )
            table_alpha = {
                slot: rigel_assets.classify_png_alpha(
                    (assets / str(texture)).read_bytes(), str(texture)
                )
                for slot, texture in table["textures"].items()
            }
            self.assertEqual(
                table_alpha,
                {
                    "border": rigel_assets.TextureAlphaClass.FULLY_OPAQUE,
                    "top": rigel_assets.TextureAlphaClass.FRACTIONAL,
                },
            )
            self.assertEqual(generated_blocks["base:grass_blades"]["collision"], "none")
            self.assertEqual(generated_blocks["base:asphalt"]["collision"], "full")
            self.assertEqual(
                generated_blocks["base:asphalt[slab_type=bottom]"]["collision"],
                {"boxes": [[0.0, 0.0, 0.0, 1.0, 0.5, 1.0]]},
            )
            self.assertEqual(
                generated_blocks[
                    "base:asphalt[stair_type=bottom_NegX]"
                ]["collision"],
                {
                    "boxes": [
                        [0.0, 0.5, 0.0, 0.5, 1.0, 1.0],
                        [0.0, 0.0, 0.0, 1.0, 0.5, 1.0],
                    ]
                },
            )
            piston_head = generated_blocks[
                "base:piston[direction=PosX,type=push,part=head]"
            ]["collision"]
            self.assertEqual(
                piston_head,
                {
                    "boxes": [
                        [0.75, 0.0, 0.0, 1.0, 1.0, 1.0],
                        [-0.25, 0.375, 0.375, 0.75, 0.625, 0.625],
                    ]
                },
            )
            self.assertEqual(
                first["output_tree_sha256"],
                "51502c1020cc216a8f2a2b887a966ffc8ef39ba29c8c369cd453271ad2cd81bc",
            )


class _MissingTextureModelResolver:
    def resolve(self, reference: str) -> rigel_assets.ResolvedModel:
        return rigel_assets.ResolvedModel(
            {"all": "textures/blocks/missing.png"},
            {},
            False,
            False,
            False,
            True,
        )

    def texture_alpha_class(
        self, reference: str, context: str
    ) -> rigel_assets.TextureAlphaClass:
        return rigel_assets.TextureAlphaClass.FULLY_OPAQUE


class _TransparentModelResolver:
    def resolve(self, reference: str) -> rigel_assets.ResolvedModel:
        return rigel_assets.ResolvedModel(
            {"all": "textures/blocks/test.png"},
            {},
            True,
            False,
            False,
            True,
        )

    def texture_alpha_class(
        self, reference: str, context: str
    ) -> rigel_assets.TextureAlphaClass:
        return rigel_assets.TextureAlphaClass.FULLY_OPAQUE


class _DirectionalModelResolver:
    def resolve(self, reference: str) -> rigel_assets.ResolvedModel:
        aliases = {
            source: rigel_assets.VECTOR_FACES[vector]
            for source, vector in rigel_assets.FACE_VECTORS.items()
        }
        textures = {
            alias: f"textures/blocks/{alias}.png"
            for alias in aliases.values()
        }
        return rigel_assets.ResolvedModel(
            textures,
            aliases,
            False,
            False,
            False,
            True,
        )

    def texture_alpha_class(
        self, reference: str, context: str
    ) -> rigel_assets.TextureAlphaClass:
        return rigel_assets.TextureAlphaClass.FULLY_OPAQUE


if __name__ == "__main__":
    unittest.main()
