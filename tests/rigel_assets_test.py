from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
from unittest import mock
import zlib
import zipfile


sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import rigel_assets


def write_jar(path: Path, entries: dict[str, bytes] | None = None) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in (entries or {"base/version.txt": b"test"}).items():
            archive.writestr(name, data)


def encoded_json(value: object) -> bytes:
    return json.dumps(value).encode("utf-8")


def synthetic_png(width: int = 16, height: int = 16) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    pixels = b"".join(b"\0" + b"\xff\xff\xff\xff" * width for _ in range(height))
    return (
        rigel_assets.PNG_SIGNATURE
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
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
            {"generation": "old"},
        )
        staging = root / ".rigel/.assets-staging-fixture"
        rigel_assets.write_output(staging, "new.txt", b"new")
        return staging, {"generation": "new"}

    def _assert_publication(self, root: Path, generation: str) -> None:
        assets = root / rigel_assets.GENERATED_ASSETS_RELATIVE_PATH
        provenance = json.loads(
            (root / rigel_assets.PROVENANCE_RELATIVE_PATH).read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(provenance, {"generation": generation})
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

            with mock.patch.object(
                rigel_assets,
                "atomic_write_json",
                side_effect=OSError("injected provenance write failure"),
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

            with mock.patch.object(
                rigel_assets,
                "atomic_write_json",
                side_effect=OSError("injected first provenance write failure"),
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
            real_replace = os.replace
            restoration_attempts = 0

            def publish_then_fail(destination: Path, value: dict[str, object]) -> None:
                real_atomic_write(destination, value)
                raise OSError("injected post-publication failure")

            def fail_first_restore(source: object, destination: object) -> None:
                nonlocal restoration_attempts
                source_path = Path(source)
                if (
                    Path(destination) == provenance_path
                    and source_path.parent.name.startswith(".assets-previous-")
                ):
                    restoration_attempts += 1
                    if restoration_attempts == 1:
                        raise OSError("injected provenance restore failure")
                real_replace(source, destination)

            with mock.patch.object(
                rigel_assets, "atomic_write_json", side_effect=publish_then_fail
            ), mock.patch.object(
                rigel_assets.os, "replace", side_effect=fail_first_restore
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
            cleanup_attempts = 0

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
                side_effect=OSError("injected provenance write failure"),
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

    def test_model_transparency_and_fluid_self_culling_map_to_runtime_fields(self) -> None:
        resolver = _TransparentModelResolver()
        output = rigel_assets.render_block_yaml(
            "test:fluid",
            {"modelName": "unused", "isFluid": True},
            resolver,
            "fixture",
        ).decode()
        self.assertIn("opaque: false", output)
        self.assertIn("layer: transparent", output)
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


class SynchronizationTest(unittest.TestCase):
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
            self.assertEqual(first["importer_schema"], 3)
            self.assertEqual(first["counts"]["blocks"], 4)
            self.assertEqual(
                first["output_tree_sha256"],
                rigel_assets.sha256_tree(root / ".rigel/assets"),
            )

    def test_source_change_replaces_tree_instead_of_accumulating(self) -> None:
        first_entries = synthetic_block_entries()
        first_entries["base/sounds/stale.ogg"] = b"old"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar = root / "fixture.jar"
            write_jar(jar, first_entries)
            rigel_assets.synchronize(root, jar, required_identifiers=("test:stone",))
            stale = root / ".rigel/assets/sounds/stale.ogg"
            self.assertTrue(stale.is_file())

            second_entries = synthetic_block_entries()
            second_entries["base/version.txt"] = b"second"
            write_jar(jar, second_entries)
            _, changed = rigel_assets.synchronize(
                root, jar, required_identifiers=("test:stone",)
            )

            self.assertTrue(changed)
            self.assertFalse(stale.exists())

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


if __name__ == "__main__":
    unittest.main()
