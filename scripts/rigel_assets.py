#!/usr/bin/env python3
"""Prepare local Cosmic Reach assets for Rigel without redistributing them."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import shutil
import stat
import sys
import tempfile
import zipfile


TOOL_NAME = "rigel_assets"
JAR_ENVIRONMENT_VARIABLE = "RIGEL_COSMIC_REACH_JAR"
STAGED_JAR_RELATIVE_PATH = Path(".rigel/source/Cosmic-Reach.jar")
GENERATED_ASSETS_RELATIVE_PATH = Path(".rigel/assets")
PROVENANCE_RELATIVE_PATH = Path(".rigel/cosmic-reach-import.json")
PROVENANCE_SCHEMA = 1
IMPORTER_SCHEMA = 1
SOURCE_PREFIX = "base/"


class AssetImportError(RuntimeError):
    """An actionable local asset preparation failure."""


def repository_root() -> Path:
    return Path(__file__).resolve().parent.parent


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def importer_sha256() -> str:
    return sha256_file(Path(__file__).resolve())


def normalize_archive_path(raw_name: str) -> PurePosixPath:
    if not raw_name or "\\" in raw_name or "\x00" in raw_name:
        raise AssetImportError(f"unsafe JAR entry path: {raw_name!r}")
    if raw_name.startswith("/"):
        raise AssetImportError(f"unsafe absolute JAR entry path: {raw_name!r}")
    name = raw_name[:-1] if raw_name.endswith("/") else raw_name
    path = PurePosixPath(name)
    if not path.parts or any(part in ("", ".", "..") for part in path.parts):
        raise AssetImportError(f"unsafe JAR entry path: {raw_name!r}")
    if ":" in path.parts[0]:
        raise AssetImportError(f"unsafe drive-qualified JAR entry path: {raw_name!r}")
    return path


def indexed_archive(archive: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
    entries: dict[str, zipfile.ZipInfo] = {}
    for info in archive.infolist():
        path = normalize_archive_path(info.filename)
        normalized = path.as_posix()
        if info.is_dir():
            continue
        mode = info.external_attr >> 16
        if mode and stat.S_ISLNK(mode):
            raise AssetImportError(f"symbolic-link JAR entry is not supported: {normalized}")
        if normalized in entries:
            raise AssetImportError(f"duplicate JAR entry path: {normalized}")
        entries[normalized] = info
    return entries


def _strip_json_comments(text: str) -> str:
    output: list[str] = []
    index = 0
    in_string = False
    escaped = False
    line_comment = False
    block_comment = False
    while index < len(text):
        character = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if line_comment:
            if character in "\r\n":
                line_comment = False
                output.append(character)
            index += 1
            continue
        if block_comment:
            if character == "*" and following == "/":
                block_comment = False
                index += 2
            else:
                index += 1
            continue
        if in_string:
            output.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            index += 1
            continue
        if character == '"':
            in_string = True
            output.append(character)
            index += 1
        elif character == "/" and following == "/":
            line_comment = True
            index += 2
        elif character == "/" and following == "*":
            block_comment = True
            index += 2
        else:
            output.append(character)
            index += 1
    if in_string or block_comment:
        raise AssetImportError("unterminated string or comment in JSON input")
    return "".join(output)


def load_relaxed_json(data: bytes, source: str) -> object:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AssetImportError(f"{source}: JSON is not UTF-8: {error}") from error
    cleaned = _strip_json_comments(text)
    cleaned = re.sub(r",\s*([}\]])", r"\1", cleaned)
    # Cosmic Reach 0.6.1 has one known missing comma between adjacent objects in
    # all_stairs_seamed.json. Accept that narrow legacy JSON defect; other parse
    # errors still fail closed.
    cleaned = re.sub(r"}(\s*){", r"},\1{", cleaned)
    try:
        return json.loads(cleaned)
    except json.JSONDecodeError as error:
        raise AssetImportError(f"{source}: malformed JSON: {error}") from error


def deterministic_json(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def _strip_base_namespace(reference: str, context: str) -> str:
    if not reference.startswith("base:"):
        raise AssetImportError(f"{context}: expected a base: resource, got {reference!r}")
    path = reference.removeprefix("base:")
    normalize_archive_path(path)
    return path


def normalize_entity_model(data: bytes, source: str, identifier: str) -> bytes:
    document = load_relaxed_json(data, source)
    if not isinstance(document, dict):
        raise AssetImportError(f"{source}: entity model root must be an object")
    textures = document.get("textures", {})
    if not isinstance(textures, dict):
        raise AssetImportError(f"{source}: entity model textures must be an object")
    normalized_textures: dict[str, str] = {}
    for name, reference in textures.items():
        if not isinstance(name, str) or not isinstance(reference, str):
            raise AssetImportError(f"{source}: entity texture entries must be strings")
        normalized_textures[name] = _strip_base_namespace(reference, source)
    document["id"] = identifier
    document["lighting"] = "unlit"
    document["textures"] = normalized_textures
    return deterministic_json(document)


def normalize_entity_animation(data: bytes, source: str, identifier: str) -> bytes:
    document = load_relaxed_json(data, source)
    if not isinstance(document, dict) or not isinstance(document.get("animations"), dict):
        raise AssetImportError(f"{source}: animation root must contain an animations object")
    document["id"] = identifier
    return deterministic_json(document)


def extract_direct_assets(
    archive: zipfile.ZipFile,
    entries: dict[str, zipfile.ZipInfo],
    staging: Path,
) -> dict[str, int]:
    counts = {"textures": 0, "models": 0, "animations": 0, "sounds": 0}
    for source in sorted(entries):
        destination: str | None = None
        transform = None
        identifier = ""
        if source.startswith("base/textures/") and source.endswith(".png"):
            destination = source.removeprefix("base/")
            counts["textures"] += 1
        elif source.startswith("base/models/entities/") and source.endswith(".json"):
            relative = source.removeprefix("base/models/entities/")
            # Preserve the existing flat logical IDs for planet models.
            if relative.startswith("planets/"):
                relative = PurePosixPath(relative).name
            destination = f"models/entities/{relative}"
            identifier = PurePosixPath(relative).name.removesuffix(".json")
            transform = normalize_entity_model
            counts["models"] += 1
        elif source.startswith("base/animations/entities/") and source.endswith(".json"):
            relative = source.removeprefix("base/animations/entities/")
            destination = f"animations/entities/{relative}"
            identifier = PurePosixPath(relative).name.removesuffix(".animation.json")
            transform = normalize_entity_animation
            counts["animations"] += 1
        elif source.startswith("base/sounds/") and source.endswith(".ogg"):
            destination = source.removeprefix("base/")
            counts["sounds"] += 1

        if destination is None:
            continue
        payload = archive.read(entries[source])
        if transform is not None:
            payload = transform(payload, source, identifier)
        write_output(staging, destination, payload)
    return counts


def output_path(root: Path, logical_path: str) -> Path:
    normalized = normalize_archive_path(logical_path)
    destination = root.joinpath(*normalized.parts)
    if destination == root or root not in destination.parents:
        raise AssetImportError(f"unsafe generated asset path: {logical_path!r}")
    return destination


def write_output(root: Path, logical_path: str, data: bytes) -> None:
    destination = output_path(root, logical_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)


def sha256_tree(root: Path) -> str:
    digest = hashlib.sha256()
    if not root.is_dir():
        raise AssetImportError(f"generated asset tree does not exist: {root}")
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        data = path.read_bytes()
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def atomic_write_json(destination: Path, value: dict[str, object]) -> None:
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def publish_generated_tree(root: Path, staging: Path, provenance: dict[str, object]) -> None:
    destination = root / GENERATED_ASSETS_RELATIVE_PATH
    provenance_path = root / PROVENANCE_RELATIVE_PATH
    destination.parent.mkdir(parents=True, exist_ok=True)
    backup = Path(
        tempfile.mkdtemp(prefix=".assets-previous-", dir=destination.parent)
    )
    backup.rmdir()
    previous_provenance = provenance_path.read_bytes() if provenance_path.is_file() else None
    moved_previous = False
    try:
        if destination.exists():
            os.replace(destination, backup)
            moved_previous = True
        os.replace(staging, destination)
        try:
            atomic_write_json(provenance_path, provenance)
        except Exception:
            failed = destination.with_name(f".{destination.name}.failed")
            if failed.exists():
                shutil.rmtree(failed)
            os.replace(destination, failed)
            if moved_previous:
                os.replace(backup, destination)
            if previous_provenance is None:
                provenance_path.unlink(missing_ok=True)
            else:
                provenance_path.write_bytes(previous_provenance)
            shutil.rmtree(failed)
            raise
        if moved_previous:
            shutil.rmtree(backup)
    except Exception:
        if moved_previous and backup.exists() and not destination.exists():
            os.replace(backup, destination)
        raise


def current_import_matches(root: Path, jar_digest: str) -> bool:
    provenance = read_provenance(root)
    assets = root / GENERATED_ASSETS_RELATIVE_PATH
    if not provenance or not assets.is_dir():
        return False
    if (
        provenance.get("schema") != PROVENANCE_SCHEMA
        or provenance.get("importer_schema") != IMPORTER_SCHEMA
        or provenance.get("importer_sha256") != importer_sha256()
        or provenance.get("jar_sha256") != jar_digest
    ):
        return False
    try:
        return provenance.get("output_tree_sha256") == sha256_tree(assets)
    except (AssetImportError, OSError):
        return False


def validate_jar(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise AssetImportError(f"Cosmic Reach JAR does not exist: {resolved}")
    if not zipfile.is_zipfile(resolved):
        raise AssetImportError(f"Cosmic Reach source is not a readable JAR/ZIP: {resolved}")
    return resolved


def resolve_jar(root: Path, explicit: str | Path | None = None) -> tuple[Path, str]:
    """Resolve the JAR by explicit path, environment, then canonical staging."""
    if explicit:
        return validate_jar(Path(explicit)), "explicit path"

    environment_path = os.environ.get(JAR_ENVIRONMENT_VARIABLE)
    if environment_path:
        return validate_jar(Path(environment_path)), JAR_ENVIRONMENT_VARIABLE

    staged = root / STAGED_JAR_RELATIVE_PATH
    if staged.is_file():
        return validate_jar(staged), "staged file"

    raise AssetImportError(
        "Cosmic Reach runtime assets have not been prepared.\n\n"
        "Provide a Cosmic Reach JAR using one of:\n\n"
        "  python3 scripts/rigel_assets.py stage /path/to/Cosmic-Reach.jar\n\n"
        "or\n\n"
        "  RIGEL_COSMIC_REACH_JAR=/path/to/Cosmic-Reach.jar "
        "python3 scripts/rigel_assets.py sync\n\n"
        "Source-only builds and unit tests do not require the JAR."
    )


def atomic_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        shutil.copyfile(source, temporary)
        with temporary.open("rb+") as stream:
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def stage_jar(root: Path, source: str | Path) -> Path:
    source_path = validate_jar(Path(source))
    destination = root / STAGED_JAR_RELATIVE_PATH
    if source_path == destination.resolve():
        return destination
    atomic_copy(source_path, destination)
    return destination


def read_provenance(root: Path) -> dict[str, object] | None:
    path = root / PROVENANCE_RELATIVE_PATH
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def status(root: Path, explicit: str | Path | None = None) -> int:
    try:
        jar, source = resolve_jar(root, explicit)
    except AssetImportError as error:
        print(str(error))
        return 1

    jar_digest = sha256_file(jar)
    provenance = read_provenance(root)
    generated = root / GENERATED_ASSETS_RELATIVE_PATH
    synchronized = (
        generated.is_dir()
        and provenance is not None
        and provenance.get("jar_sha256") == jar_digest
    )
    print(f"Cosmic Reach JAR: {jar} ({source})")
    print(f"JAR SHA-256: {jar_digest}")
    print(f"Generated assets: {'current' if synchronized else 'not synchronized'}")
    return 0 if synchronized else 2


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Stage and synchronize developer-provided Cosmic Reach assets."
    )
    parser.add_argument("--root", type=Path, default=repository_root(), help=argparse.SUPPRESS)
    subparsers = parser.add_subparsers(dest="command", required=True)

    stage_parser = subparsers.add_parser(
        "stage", help="copy a manually obtained JAR into .rigel/source"
    )
    stage_parser.add_argument("jar", type=Path)

    status_parser = subparsers.add_parser(
        "status", help="report JAR and generated-tree state"
    )
    status_parser.add_argument("--jar", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = args.root.expanduser().resolve()
    try:
        if args.command == "stage":
            destination = stage_jar(root, args.jar)
            print(f"Staged Cosmic Reach JAR: {destination}")
            print(f"SHA-256: {sha256_file(destination)}")
            print("Run `python3 scripts/rigel_assets.py sync` to generate assets.")
            return 0
        if args.command == "status":
            return status(root, args.jar)
    except AssetImportError as error:
        print(f"{TOOL_NAME}: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
