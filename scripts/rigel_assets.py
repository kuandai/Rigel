#!/usr/bin/env python3
"""Prepare local Cosmic Reach assets for Rigel without redistributing them."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass, field
from enum import Enum
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
from pathlib import PurePosixPath
import shutil
import stat
import struct
import sys
import tempfile
import zipfile
import zlib


TOOL_NAME = "rigel_assets"
JAR_ENVIRONMENT_VARIABLE = "RIGEL_COSMIC_REACH_JAR"
STAGED_JAR_RELATIVE_PATH = Path(".rigel/source/Cosmic-Reach.jar")
GENERATED_ASSETS_RELATIVE_PATH = Path(".rigel/assets")
PROVENANCE_RELATIVE_PATH = Path(".rigel/cosmic-reach-import.json")
PUBLICATION_LOCK_RELATIVE_PATH = Path(".rigel/assets-publication.lock")
SYNCHRONIZATION_LOCK_PREFIX = ".assets-synchronization-"
STAGING_PREFIX = ".assets-staging-"
STAGING_LEASE_SUFFIX = ".lock"
SNAPSHOT_LOCK_FILENAME = ".snapshot.lock"
SNAPSHOT_ACTIVE_GENERATION_FILENAME = ".active-generation.json"
SNAPSHOT_STAGING_SUFFIX = ".staging"
PROVENANCE_SCHEMA = 1
IMPORTER_SCHEMA = 7
BLOCK_MODEL_SUPPORT_SCHEMA = 1
BLOCK_COLLISION_SUPPORT_SCHEMA = 3
MATERIAL_LAYER_AUDIT_SCHEMA = 1
SOURCE_PREFIX = "base/"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
BLOCK_TEXTURE_SIZE = (16, 16)
BLOCK_COLLISION_MINIMUM_COORDINATE = -0.25
BLOCK_COLLISION_MAXIMUM_COORDINATE = 1.25
REQUIRED_BLOCK_IDENTIFIERS = (
    "base:dirt",
    "base:grass",
    "base:sand",
    "base:stone_shale",
    "base:water[type=source]",
)
# The pre-import Rigel snapshot exposed these bare IDs. Cosmic Reach 0.6.1
# spells their current default state with parameters; retaining an explicit
# alias prevents existing Rigel saves and code from losing a known block ID.
LEGACY_DEFAULT_STATE_ALIASES = {
    "base:hazard": {"type": "hazard"},
    "base:ice": {"axis": "Y"},
    "base:magma": {"axis": "Y"},
    "base:sandstone": {"axis": "Y"},
    "base:snow": {"axis": "Y"},
    "base:stone_basalt": {"axis": "Y"},
    "base:stone_gabbro": {"axis": "Y"},
    "base:stone_komatiite": {"axis": "Y"},
    "base:stone_limestone": {"axis": "Y"},
    "base:tree_log": {"axis": "Y"},
}
ENTITY_MODEL_COMPATIBILITY: dict[str, dict[str, object]] = {
    "drone_laser": {
        "hitbox": {"min": [-0.4, -0.4, -0.4], "max": [0.4, 0.8, 0.4]},
    },
    "incinerator": {
        "hitbox": {"min": [-0.45, 0.0, -0.45], "max": [0.45, 1.45, 0.45]},
    },
    "model_bullet_projectile": {
        "hitbox": {"min": [-0.1, -0.1, -0.1], "max": [0.1, 0.1, 0.1]},
    },
    "model_drone_interceptor": {
        "animation_set": "entity_anims/drone_interceptor",
        "default_animation": "animation.drone-interceptor.idle",
        "hitbox": {"min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
        "render_offset": [0.0, -0.375, 0.0],
    },
    "model_drone_interceptor_trap": {
        "animation_set": "entity_anims/drone_interceptor_trap",
        "default_animation": "animation.drone-interceptor-trap.idle",
        "hitbox": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 0.5, 0.5]},
    },
    "model_flame_projectile": {
        "hitbox": {"min": [-0.4, -0.4, -0.4], "max": [0.4, 0.4, 0.4]},
    },
    "model_laser_projectile": {
        "hitbox": {"min": [-0.1, -0.1, -0.1], "max": [0.1, 0.1, 0.1]},
    },
    "planteater": {
        "hitbox": {"min": [-0.4, 0.0, -0.4], "max": [0.4, 0.75, 0.4]},
    },
    "player": {
        "hitbox": {"min": [-0.25, 0.0, -0.25], "max": [0.25, 1.9, 0.25]},
    },
}


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


@contextmanager
def _source_jar_snapshot(root: Path, source: Path):
    workspace = root / ".rigel"
    workspace.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with tempfile.TemporaryFile(
        prefix=".source-jar-", suffix=".tmp", dir=workspace
    ) as snapshot:
        with source.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                snapshot.write(chunk)
                digest.update(chunk)
        snapshot.flush()
        snapshot.seek(0)
        if not zipfile.is_zipfile(snapshot):
            raise AssetImportError(
                f"Cosmic Reach source is not a readable JAR/ZIP: {source}"
            )
        snapshot.seek(0)
        yield snapshot, digest.hexdigest()


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


def _normalize_relaxed_json_punctuation(text: str) -> str:
    output: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(text):
        character = text[index]
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
            continue

        lookahead = index + 1
        while lookahead < len(text) and text[lookahead].isspace():
            lookahead += 1
        following = text[lookahead] if lookahead < len(text) else ""
        if character == "," and following in ("}", "]"):
            index += 1
            continue
        output.append(character)
        if character == "}" and following == "{":
            output.append(",")
        index += 1
    return "".join(output)


def load_relaxed_json(data: bytes, source: str) -> object:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AssetImportError(f"{source}: JSON is not UTF-8: {error}") from error
    cleaned = _normalize_relaxed_json_punctuation(_strip_json_comments(text))
    # Cosmic Reach 0.6.1 has one known missing comma between adjacent objects in
    # all_stairs_seamed.json. Accept that narrow legacy JSON defect; other parse
    # errors still fail closed.
    try:
        return json.loads(cleaned)
    except json.JSONDecodeError as error:
        raise AssetImportError(f"{source}: malformed JSON: {error}") from error


def deterministic_json(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def png_dimensions(data: bytes, source: str) -> tuple[int, int]:
    if (
        len(data) < 24
        or data[:8] != PNG_SIGNATURE
        or data[8:12] != b"\x00\x00\x00\r"
        or data[12:16] != b"IHDR"
    ):
        raise AssetImportError(f"{source}: malformed PNG header")
    width, height = struct.unpack(">II", data[16:24])
    if width == 0 or height == 0:
        raise AssetImportError(f"{source}: PNG dimensions must be non-zero")
    return width, height


def _paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def _png_samples(scanline: bytes, bit_depth: int) -> list[int]:
    if bit_depth == 8:
        return list(scanline)
    if bit_depth == 16:
        return [
            int.from_bytes(scanline[offset : offset + 2], "big")
            for offset in range(0, len(scanline), 2)
        ]
    mask = (1 << bit_depth) - 1
    return [
        (scanline[index * bit_depth // 8]
         >> (8 - bit_depth - index * bit_depth % 8)) & mask
        for index in range(len(scanline) * 8 // bit_depth)
    ]


class TextureAlphaClass(Enum):
    FULLY_OPAQUE = "fully_opaque"
    BINARY = "binary"
    FRACTIONAL = "fractional"


def classify_png_alpha(data: bytes, source: str) -> TextureAlphaClass:
    width, height = png_dimensions(data, source)
    if len(data) < 33:
        raise AssetImportError(f"{source}: malformed PNG header")
    bit_depth, color_type, compression, filtering, interlace = struct.unpack(
        ">BBBBB", data[24:29]
    )
    valid_bit_depths = {
        0: (1, 2, 4, 8, 16),
        2: (8, 16),
        3: (1, 2, 4, 8),
        4: (8, 16),
        6: (8, 16),
    }
    if (
        color_type not in valid_bit_depths
        or bit_depth not in valid_bit_depths[color_type]
    ):
        raise AssetImportError(
            f"{source}: unsupported PNG color type or bit depth"
        )
    if compression != 0 or filtering != 0 or interlace != 0:
        raise AssetImportError(
            f"{source}: unsupported PNG compression, filtering, or interlace"
        )

    compressed = bytearray()
    transparency: bytes | None = None
    palette_entries: int | None = None
    offset = len(PNG_SIGNATURE)
    found_header = False
    found_data = False
    found_end = False
    data_closed = False
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            raise AssetImportError(f"{source}: truncated PNG chunk")
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        expected_crc = struct.unpack(">I", data[offset + 8 + length : chunk_end])[0]
        actual_crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise AssetImportError(f"{source}: PNG chunk checksum mismatch")
        if len(kind) != 4 or not all(
            ord("A") <= value <= ord("Z") or ord("a") <= value <= ord("z")
            for value in kind
        ):
            raise AssetImportError(f"{source}: invalid PNG chunk type")
        if kind[2] & 0x20:
            raise AssetImportError(
                f"{source}: PNG chunk type has a lowercase reserved byte"
            )
        if not found_header and kind != b"IHDR":
            raise AssetImportError(f"{source}: PNG header is not the first chunk")
        if kind == b"IHDR":
            if found_header or length != 13 or offset != len(PNG_SIGNATURE):
                raise AssetImportError(f"{source}: malformed PNG header chunk")
            found_header = True
        elif kind == b"PLTE":
            if (
                found_data
                or palette_entries is not None
                or transparency is not None
            ):
                raise AssetImportError(f"{source}: malformed PNG palette")
            if length == 0 or length % 3 != 0 or length > 256 * 3:
                raise AssetImportError(f"{source}: malformed PNG palette")
            palette_entries = length // 3
        elif kind == b"IDAT":
            if data_closed:
                raise AssetImportError(f"{source}: PNG image data is not consecutive")
            found_data = True
            compressed.extend(payload)
        elif kind == b"tRNS":
            if found_data or transparency is not None:
                raise AssetImportError(f"{source}: malformed PNG transparency")
            if color_type == 3 and palette_entries is None:
                raise AssetImportError(
                    f"{source}: indexed PNG transparency precedes its palette"
                )
            transparency = payload
        elif kind == b"IEND":
            if length != 0 or not found_data:
                raise AssetImportError(f"{source}: malformed PNG end chunk")
            found_end = True
            if chunk_end != len(data):
                raise AssetImportError(f"{source}: trailing data after PNG end chunk")
            break
        elif kind[0] & 0x20 == 0:
            raise AssetImportError(
                f"{source}: unsupported critical PNG chunk {kind.decode('ascii')}"
            )
        if found_data and kind != b"IDAT":
            data_closed = True
        offset = chunk_end
    if not found_header or not compressed or not found_end:
        raise AssetImportError(f"{source}: PNG image data is incomplete")

    if color_type == 3:
        if palette_entries is None or palette_entries > (1 << bit_depth):
            raise AssetImportError(f"{source}: malformed indexed PNG palette")
        if transparency is not None and (
            not transparency or len(transparency) > palette_entries
        ):
            raise AssetImportError(f"{source}: malformed indexed PNG transparency")
    elif color_type == 0 and palette_entries is not None:
        raise AssetImportError(f"{source}: unexpected grayscale PNG palette")
    elif color_type == 4 and palette_entries is not None:
        raise AssetImportError(f"{source}: unexpected grayscale-alpha PNG palette")

    samples_per_pixel = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    row_bytes = (width * samples_per_pixel * bit_depth + 7) // 8
    filter_bytes_per_pixel = max(
        1, (samples_per_pixel * bit_depth + 7) // 8
    )
    expected_size = height * (row_bytes + 1)
    try:
        decompressor = zlib.decompressobj()
        filtered = decompressor.decompress(
            bytes(compressed), expected_size + 1
        )
    except zlib.error as error:
        raise AssetImportError(f"{source}: malformed PNG image data") from error
    if (
        len(filtered) != expected_size
        or not decompressor.eof
        or decompressor.unconsumed_tail
        or decompressor.unused_data
    ):
        raise AssetImportError(
            f"{source}: PNG scanline data has the wrong size"
        )

    transparent_gray: int | None = None
    transparent_rgb: tuple[int, int, int] | None = None
    if color_type == 0 and transparency is not None:
        if len(transparency) != 2:
            raise AssetImportError(
                f"{source}: malformed grayscale PNG transparency"
            )
        transparent_gray = int.from_bytes(transparency, "big")
        if transparent_gray >= (1 << bit_depth):
            raise AssetImportError(
                f"{source}: grayscale PNG transparency is out of range"
            )
    elif color_type == 2 and transparency is not None:
        if len(transparency) != 6:
            raise AssetImportError(
                f"{source}: malformed truecolor PNG transparency"
            )
        transparent_rgb = struct.unpack(">HHH", transparency)
        if any(sample >= (1 << bit_depth) for sample in transparent_rgb):
            raise AssetImportError(
                f"{source}: truecolor PNG transparency is out of range"
            )
    elif color_type not in (0, 2, 3) and transparency is not None:
        raise AssetImportError(
            f"{source}: unexpected PNG transparency chunk"
        )

    alpha_class = TextureAlphaClass.FULLY_OPAQUE
    maximum_sample = (1 << bit_depth) - 1

    def record_alpha(alpha: int, opaque_value: int = maximum_sample) -> None:
        nonlocal alpha_class
        if alpha not in (0, opaque_value):
            alpha_class = TextureAlphaClass.FRACTIONAL
        elif alpha == 0 and alpha_class == TextureAlphaClass.FULLY_OPAQUE:
            alpha_class = TextureAlphaClass.BINARY

    previous = bytearray(row_bytes)
    for row_index in range(height):
        start = row_index * (row_bytes + 1)
        filter_type = filtered[start]
        if filter_type > 4:
            raise AssetImportError(f"{source}: invalid PNG scanline filter")
        encoded = filtered[start + 1 : start + 1 + row_bytes]
        row = bytearray(row_bytes)
        for index, value in enumerate(encoded):
            left = (
                row[index - filter_bytes_per_pixel]
                if index >= filter_bytes_per_pixel
                else 0
            )
            above = previous[index]
            upper_left = (
                previous[index - filter_bytes_per_pixel]
                if index >= filter_bytes_per_pixel
                else 0
            )
            predictor = (
                0 if filter_type == 0
                else left if filter_type == 1
                else above if filter_type == 2
                else (left + above) // 2 if filter_type == 3
                else _paeth_predictor(left, above, upper_left)
            )
            row[index] = (value + predictor) & 0xFF
        samples = _png_samples(row, bit_depth)
        if color_type == 6:
            for index in range(3, width * 4, 4):
                record_alpha(samples[index])
        elif color_type == 4:
            for index in range(1, width * 2, 2):
                record_alpha(samples[index])
        elif color_type == 3:
            assert palette_entries is not None
            for sample in samples[:width]:
                if sample >= palette_entries:
                    raise AssetImportError(
                        f"{source}: indexed PNG sample is outside its palette"
                    )
                record_alpha(
                    transparency[sample]
                    if transparency is not None and sample < len(transparency)
                    else 0xFF,
                    0xFF,
                )
        elif color_type == 0 and transparent_gray is not None:
            for sample in samples[:width]:
                record_alpha(0 if sample == transparent_gray else maximum_sample)
        elif color_type == 2 and transparent_rgb is not None:
            for index in range(0, width * 3, 3):
                record_alpha(
                    0
                    if tuple(samples[index : index + 3]) == transparent_rgb
                    else maximum_sample
                )
        previous = row
    return alpha_class


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
    document["model_scale"] = 0.0625
    document["textures"] = normalized_textures
    document.update(ENTITY_MODEL_COMPATIBILITY.get(identifier, {}))
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
    diagnostics: list[str] | None = None,
) -> dict[str, int]:
    counts = {"textures": 0, "models": 0, "animations": 0, "sounds": 0}
    destinations: dict[str, str] = {}
    for source in sorted(entries):
        destination: str | None = None
        transform = None
        identifier = ""
        if source.startswith("base/textures/") and source.endswith(".png"):
            destination = source.removeprefix("base/")
        elif source.startswith("base/models/entities/") and source.endswith(".json"):
            relative = source.removeprefix("base/models/entities/")
            # Preserve the existing flat logical IDs for planet models.
            if relative.startswith("planets/"):
                relative = PurePosixPath(relative).name
            destination = f"models/entities/{relative}"
            identifier = relative.removesuffix(".json")
            transform = normalize_entity_model
        elif source.startswith("base/animations/entities/") and source.endswith(".json"):
            relative = source.removeprefix("base/animations/entities/")
            destination = f"animations/entities/{relative}"
            identifier = PurePosixPath(relative).name.removesuffix(".animation.json")
            transform = normalize_entity_animation
        elif source.startswith("base/sounds/") and source.endswith(".ogg"):
            destination = source.removeprefix("base/")

        if destination is None:
            continue
        previous_source = destinations.get(destination)
        if previous_source is not None:
            raise AssetImportError(
                f"duplicate generated logical path {destination}: "
                f"{previous_source} and {source}"
            )
        destinations[destination] = source
        payload = archive.read(entries[source])
        if transform is not None:
            payload = transform(payload, source, identifier)
        if source.startswith("base/models/entities/"):
            model = _expect_object(load_relaxed_json(payload, source), source)
            textures = _expect_object(model.get("textures", {}), f"{source}.textures")
            missing = sorted(
                reference
                for reference in textures.values()
                if isinstance(reference, str) and f"base/{reference}" not in entries
            )
            if missing:
                message = (
                    f"omitted optional entity model {source}: source JAR lacks "
                    + ", ".join(missing)
                )
                if diagnostics is not None:
                    diagnostics.append(message)
                continue
        write_output(staging, destination, payload)
        if destination.startswith("textures/"):
            counts["textures"] += 1
        elif destination.startswith("models/entities/"):
            counts["models"] += 1
        elif destination.startswith("animations/entities/"):
            counts["animations"] += 1
        elif destination.startswith("sounds/"):
            counts["sounds"] += 1
    return counts


BLOCK_ROOT_KEYS = {
    "stringId",
    "blockStates",
    "defaultProperties",
    "defaultParams",
    "blockEntityId",
    "blockEntityParams",
}
BLOCK_PROPERTY_KEYS = {
    "modelName",
    "blockEventsId",
    "langKey",
    "stateGenerators",
    "catalogHidden",
    "tags",
    "lightLevelRed",
    "lightLevelGreen",
    "lightLevelBlue",
    "rotation",
    "hardness",
    "dropId",
    "allowSwapping",
    "lightAttenuation",
    "canDrop",
    "footstepSoundBank",
    "isOpaque",
    "dropParams",
    "placementRules",
    "itemIcon",
    "intProperties",
    "refractiveIndex",
    "rotateTopBottom",
    "canRaycastForPlaceOn",
    "canRaycastForReplace",
    "walkThrough",
    "swapGroupId",
    "canPlace",
    "canRaycastForBreak",
    "bounciness",
    "isFluid",
    "viscosity",
    "friction",
}
GENERATOR_KEYS = {"stringId", "params", "overrides", "modelName", "include"}
GENERATOR_OVERRIDE_KEYS = BLOCK_PROPERTY_KEYS
MODEL_ROOT_KEYS = {
    "textures",
    "parent",
    "cuboids",
    "isTransparent",
    "cullsSelf",
    "planes",
    "vertShader",
    "fragShader",
}
MODEL_TEXTURE_KEYS = {"fileName", "emissivefileName", "uv"}
MODEL_CUBOID_KEYS = {"localBounds", "faces", "inflate"}
MODEL_FACE_KEYS = {
    "uv",
    "texture",
    "ambientocclusion",
    "cullFace",
    "uvRotation",
    "shadingFace",
}
FACE_VECTORS = {
    "localPosX": (1, 0, 0),
    "localNegX": (-1, 0, 0),
    "localPosY": (0, 1, 0),
    "localNegY": (0, -1, 0),
    "localPosZ": (0, 0, 1),
    "localNegZ": (0, 0, -1),
}
VECTOR_FACES = {
    (1, 0, 0): "pos_x",
    (-1, 0, 0): "neg_x",
    (0, 1, 0): "pos_y",
    (0, -1, 0): "neg_y",
    (0, 0, 1): "pos_z",
    (0, 0, -1): "neg_z",
}
SUPPORTED_BLOCK_ORIENTATIONS = {
    (0, 0, 0),
    (90, 0, 0),
    (270, 0, 0),
    (0, 90, 0),
    (0, 180, 0),
    (0, 270, 0),
    (0, 0, 90),
}


def _expect_object(value: object, context: str) -> dict[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise AssetImportError(f"{context}: expected an object")
    return value


def _reject_unknown_keys(value: dict[str, object], allowed: set[str], context: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise AssetImportError(f"{context}: unsupported fields: {', '.join(unknown)}")


def _resource_archive_path(reference: str, context: str) -> str:
    if not isinstance(reference, str) or not reference.startswith("base:"):
        raise AssetImportError(f"{context}: expected a base: resource reference")
    path = f"base/{reference.removeprefix('base:')}"
    normalize_archive_path(path)
    return path


def _parse_parameters(value: object, context: str) -> dict[str, str]:
    mapping = _expect_object(value, context)
    result: dict[str, str] = {}
    for name, parameter in mapping.items():
        if not isinstance(parameter, (str, int, float, bool)):
            raise AssetImportError(f"{context}.{name}: parameter must be scalar")
        if isinstance(parameter, bool):
            result[name] = "true" if parameter else "false"
        else:
            result[name] = str(parameter)
    return result


def _parameters_from_state_name(name: str, context: str) -> dict[str, str]:
    if name == "default":
        return {}
    result: dict[str, str] = {}
    for assignment in name.split(","):
        if "=" not in assignment:
            raise AssetImportError(f"{context}: invalid state name {name!r}")
        key, value = assignment.split("=", 1)
        if not key or not value or key in result:
            raise AssetImportError(f"{context}: invalid state name {name!r}")
        result[key] = value
    return result


def _block_identifier(base: str, parameters: dict[str, str]) -> str:
    if not parameters:
        return base
    body = ",".join(f"{key}={value}" for key, value in parameters.items())
    return f"{base}[{body}]"


def _generated_block_filename(identifier: str) -> str:
    name = identifier.removeprefix("base:") if identifier.startswith("base:") else identifier.replace(":", "__", 1)
    if not name or "/" in name or "\\" in name or name in (".", ".."):
        raise AssetImportError(f"unsafe block identifier for output: {identifier!r}")
    return f"blocks/{name}.yaml"


def _block_orientation(value: object, context: str) -> tuple[int, int, int]:
    if value is None:
        return 0, 0, 0
    if (
        not isinstance(value, list)
        or len(value) != 3
        or not all(
            isinstance(angle, int) and not isinstance(angle, bool)
            for angle in value
        )
    ):
        raise AssetImportError(
            f"{context}: rotation must contain three integer angles"
        )
    orientation = tuple(value)
    if orientation not in SUPPORTED_BLOCK_ORIENTATIONS:
        raise AssetImportError(
            f"{context}: rotation is not present in the supported block-state set"
        )
    return orientation


@dataclass(frozen=True)
class ResolvedFace:
    texture_alias: str
    uv: tuple[float, float, float, float]
    rotation: int
    ambient_occlusion: bool
    cull_face: bool
    shading_face: str | None


@dataclass(frozen=True)
class ResolvedCuboid:
    bounds: tuple[float, float, float, float, float, float]
    faces: tuple[tuple[str, ResolvedFace], ...]


@dataclass(frozen=True)
class ResolvedGeometry:
    identifier: str
    output_path: str
    source: str
    cuboids: tuple[ResolvedCuboid, ...]
    plane_count: int = 0

    def texture_aliases(self) -> tuple[str, ...]:
        return tuple(sorted(
            {
                face.texture_alias
                for cuboid in self.cuboids
                for unused_name, face in cuboid.faces
            }
        ))


@dataclass(frozen=True)
class ResolvedModel:
    textures: dict[str, str]
    face_aliases: dict[str, str]
    transparent: bool
    culls_self: bool
    empty: bool
    full_cube: bool
    geometry: ResolvedGeometry | None = None


@dataclass(frozen=True)
class ResolvedCollisionShape:
    kind: str
    boxes: tuple[tuple[float, float, float, float, float, float], ...] = ()


@dataclass
class BlockCollisionImportAudit:
    states: dict[str, ResolvedCollisionShape] = field(default_factory=dict)

    def record(
        self, identifier: str, collision: ResolvedCollisionShape
    ) -> None:
        if identifier in self.states:
            raise AssetImportError(
                f"duplicate collision audit identifier: {identifier}"
            )
        self.states[identifier] = collision

    def discard(self, identifiers: list[str]) -> None:
        for identifier in identifiers:
            self.states.pop(identifier, None)

    def provenance(self) -> dict[str, object]:
        report: dict[str, object] = {
            "schema": BLOCK_COLLISION_SUPPORT_SCHEMA,
            "empty": sum(
                collision.kind == "empty"
                for collision in self.states.values()
            ),
            "full": sum(
                collision.kind == "full"
                for collision in self.states.values()
            ),
            "single_partial": sum(
                collision.kind == "single_partial"
                for collision in self.states.values()
            ),
            "multi_box": sum(
                collision.kind == "multi_box"
                for collision in self.states.values()
            ),
            "exact_derived": len(self.states),
            "conservative_fallback": 0,
            "ambiguous": 0,
        }
        validate_block_collision_import_report(report)
        return report


def _oriented_collision_bounds(
    bounds: tuple[float, float, float, float, float, float],
    orientation: tuple[int, int, int],
) -> tuple[float, float, float, float, float, float]:
    min_x, min_y, min_z, max_x, max_y, max_z = bounds
    if orientation == (0, 0, 0):
        result = bounds
    elif orientation == (90, 0, 0):
        result = (
            min_x, min_z, 1.0 - max_y,
            max_x, max_z, 1.0 - min_y,
        )
    elif orientation == (270, 0, 0):
        result = (
            min_x, 1.0 - max_z, min_y,
            max_x, 1.0 - min_z, max_y,
        )
    elif orientation == (0, 90, 0):
        result = (
            1.0 - max_z, min_y, min_x,
            1.0 - min_z, max_y, max_x,
        )
    elif orientation == (0, 180, 0):
        result = (
            1.0 - max_x, min_y, 1.0 - max_z,
            1.0 - min_x, max_y, 1.0 - min_z,
        )
    elif orientation == (0, 270, 0):
        result = (
            min_z, min_y, 1.0 - max_x,
            max_z, max_y, 1.0 - min_x,
        )
    elif orientation == (0, 0, 90):
        result = (
            min_y, 1.0 - max_x, min_z,
            max_y, 1.0 - min_x, max_z,
        )
    else:
        raise AssertionError(f"unsupported normalized orientation: {orientation}")
    return tuple(0.0 if coordinate == 0.0 else coordinate for coordinate in result)


def _resolved_collision_shape(
    model: ResolvedModel,
    properties: dict[str, object],
    context: str,
) -> ResolvedCollisionShape:
    walk_through = properties.get("walkThrough", False)
    if not isinstance(walk_through, bool):
        raise AssetImportError(f"{context}.walkThrough must be a boolean")
    if walk_through:
        return ResolvedCollisionShape("empty")

    if model.full_cube:
        return ResolvedCollisionShape("full")
    geometry = model.geometry
    if geometry is None or model.empty:
        return ResolvedCollisionShape("empty")
    if geometry.plane_count:
        raise AssetImportError(
            f"{context}: explicit plane geometry is not supported"
        )

    orientation = _block_orientation(
        properties.get("rotation"), f"{context}.rotation"
    )
    boxes: list[tuple[float, float, float, float, float, float]] = []
    for cuboid_index, cuboid in enumerate(geometry.cuboids):
        if any(
            cuboid.bounds[axis] == cuboid.bounds[axis + 3]
            for axis in range(3)
        ):
            continue
        bounds = _oriented_collision_bounds(cuboid.bounds, orientation)
        for coordinate_index, coordinate in enumerate(bounds):
            if (
                coordinate < BLOCK_COLLISION_MINIMUM_COORDINATE
                or coordinate > BLOCK_COLLISION_MAXIMUM_COORDINATE
            ):
                raise AssetImportError(
                    f"{context}: collision cuboid {cuboid_index} coordinate "
                    f"{coordinate_index} exceeds the supported "
                    f"[{BLOCK_COLLISION_MINIMUM_COORDINATE}, "
                    f"{BLOCK_COLLISION_MAXIMUM_COORDINATE}] range"
                )
        if bounds in boxes:
            raise AssetImportError(
                f"{context}: duplicate positive-volume collision cuboid"
            )
        boxes.append(bounds)

    if not boxes:
        return ResolvedCollisionShape("empty")
    normalized_boxes = tuple(boxes)
    if normalized_boxes == ((0.0, 0.0, 0.0, 1.0, 1.0, 1.0),):
        return ResolvedCollisionShape("full")
    return ResolvedCollisionShape(
        "single_partial" if len(normalized_boxes) == 1 else "multi_box",
        normalized_boxes,
    )


def _render_collision_shape(collision: ResolvedCollisionShape) -> str:
    if collision.kind == "empty":
        return "none"
    if collision.kind == "full":
        return "full"
    return json.dumps(
        {"boxes": [list(box) for box in collision.boxes]},
        ensure_ascii=True,
        separators=(",", ":"),
    )


@dataclass
class BlockModelImportAudit:
    candidate_states: set[str] = field(default_factory=set)
    base_approximation_states: set[str] = field(default_factory=set)
    plane_or_mixed_omissions: set[str] = field(default_factory=set)
    nonstandard_texture_omissions: set[str] = field(default_factory=set)

    def record_geometry(
        self,
        identifiers: list[str],
        *,
        generated_candidate: bool,
        requires_cuboid_model: bool,
        has_planes: bool,
    ) -> None:
        if requires_cuboid_model:
            target = (
                self.candidate_states
                if generated_candidate
                else self.base_approximation_states
            )
            target.update(identifiers)
        if has_planes:
            self.plane_or_mixed_omissions.update(identifiers)

    def record_nonstandard_textures(self, identifiers: list[str]) -> None:
        self.nonstandard_texture_omissions.update(identifiers)

    def provenance(self) -> dict[str, object]:
        overlapping_states = (
            self.candidate_states & self.base_approximation_states
        )
        if overlapping_states:
            raise AssetImportError(
                "block states were classified as both generated candidates and "
                "base approximations: " + ", ".join(sorted(overlapping_states))
            )
        overlapping_omissions = (
            self.plane_or_mixed_omissions
            & self.nonstandard_texture_omissions
        )
        if overlapping_omissions:
            raise AssetImportError(
                "block model omission reasons overlap: "
                + ", ".join(sorted(overlapping_omissions))
            )

        omitted = (
            self.plane_or_mixed_omissions
            | self.nonstandard_texture_omissions
        )

        def omission(reason_states: set[str]) -> dict[str, object]:
            candidate_count = len(reason_states & self.candidate_states)
            base_count = len(reason_states & self.base_approximation_states)
            return {
                "block_states": sorted(reason_states),
                "candidate_states": candidate_count,
                "base_approximation_states": base_count,
                "other_states": len(reason_states) - candidate_count - base_count,
            }

        report: dict[str, object] = {
            "schema": BLOCK_MODEL_SUPPORT_SCHEMA,
            "candidate_states": len(self.candidate_states),
            "newly_recovered_states": len(self.candidate_states - omitted),
            "base_approximation_states": len(self.base_approximation_states),
            "corrected_approximations": len(
                self.base_approximation_states - omitted
            ),
            "omissions": {
                "plane_or_mixed_geometry": omission(
                    self.plane_or_mixed_omissions
                ),
                "nonstandard_texture_dimensions": omission(
                    self.nonstandard_texture_omissions
                ),
            },
        }
        validate_block_model_import_report(report)
        return report


def _requires_geometric_cuboid_model(model: ResolvedModel) -> bool:
    geometry = model.geometry
    return (
        not model.empty
        and geometry is not None
        and (
            geometry.plane_count != 0
            or len(geometry.cuboids) != 1
            or geometry.cuboids[0].bounds
            != (0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
        )
    )


def _finite_number(value: object, context: str) -> float:
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or not math.isfinite(value)
    ):
        raise AssetImportError(f"{context} must be a finite number")
    return float(value)


def _normalized_geometry_location(source: str) -> tuple[str, str]:
    prefix = "base/models/blocks/"
    suffix = ".json"
    if not source.startswith(prefix) or not source.endswith(suffix):
        raise AssetImportError(f"unsupported block model location: {source}")
    relative = source[len(prefix):-len(suffix)]
    normalize_archive_path(relative)
    return f"base:block_model/{relative}", f"models/blocks/{relative}.yaml"


def _parse_model_face(
    raw_face: object, source: str, cuboid_index: int, face_name: str
) -> ResolvedFace:
    context = f"{source}.cuboids[{cuboid_index}].faces.{face_name}"
    if face_name not in FACE_VECTORS:
        raise AssetImportError(f"{context}: unsupported model face")
    face = _expect_object(raw_face, context)
    _reject_unknown_keys(face, MODEL_FACE_KEYS, context)
    alias = face.get("texture")
    if not isinstance(alias, str) or not alias:
        raise AssetImportError(f"{context}.texture must be a non-empty string")
    raw_uv = face.get("uv")
    if not isinstance(raw_uv, list) or len(raw_uv) != 4:
        raise AssetImportError(f"{context}.uv must contain four coordinates")
    uv = tuple(
        _finite_number(value, f"{context}.uv[{index}]") / 16.0
        for index, value in enumerate(raw_uv)
    )
    if any(value < 0.0 or value > 1.0 for value in uv):
        raise AssetImportError(f"{context}.uv coordinates must be within 0..16")
    rotation = face.get("uvRotation", 0)
    if (
        not isinstance(rotation, int)
        or isinstance(rotation, bool)
        or rotation not in (0, 90, 180, 270)
    ):
        raise AssetImportError(f"{context}.uvRotation must be a quarter turn")
    ambient_occlusion = face.get("ambientocclusion", False)
    cull_face = face.get("cullFace", False)
    if not isinstance(ambient_occlusion, bool):
        raise AssetImportError(f"{context}.ambientocclusion must be a boolean")
    if not isinstance(cull_face, bool):
        raise AssetImportError(f"{context}.cullFace must be a boolean")
    raw_shading = face.get("shadingFace")
    if raw_shading is not None and raw_shading not in FACE_VECTORS:
        raise AssetImportError(f"{context}.shadingFace must be a cardinal face")
    shading_face = (
        VECTOR_FACES[FACE_VECTORS[raw_shading]]
        if isinstance(raw_shading, str)
        else None
    )
    return ResolvedFace(
        alias, uv, rotation, ambient_occlusion, cull_face, shading_face
    )


def _parse_model_geometry(
    document: dict[str, object], source: str
) -> ResolvedGeometry:
    raw_cuboids = document.get("cuboids", [])
    raw_planes = document.get("planes", [])
    if not isinstance(raw_cuboids, list):
        raise AssetImportError(f"{source}.cuboids must be an array")
    if not isinstance(raw_planes, list):
        raise AssetImportError(f"{source}.planes must be an array")
    cuboids: list[ResolvedCuboid] = []
    for cuboid_index, raw_cuboid in enumerate(raw_cuboids):
        context = f"{source}.cuboids[{cuboid_index}]"
        cuboid = _expect_object(raw_cuboid, context)
        _reject_unknown_keys(cuboid, MODEL_CUBOID_KEYS, context)
        raw_bounds = cuboid.get("localBounds")
        if not isinstance(raw_bounds, list) or len(raw_bounds) != 6:
            raise AssetImportError(
                f"{context}.localBounds must contain six coordinates"
            )
        source_bounds = tuple(
            _finite_number(value, f"{context}.localBounds[{index}]")
            for index, value in enumerate(raw_bounds)
        )
        inflate = _finite_number(cuboid.get("inflate", 0), f"{context}.inflate")
        bounds = tuple(
            (value + (-inflate if index < 3 else inflate)) / 16.0
            for index, value in enumerate(source_bounds)
        )
        for axis in range(3):
            if bounds[axis] > bounds[axis + 3]:
                raise AssetImportError(
                    f"{context}.localBounds minimum exceeds maximum"
                )
        raw_faces = _expect_object(cuboid.get("faces"), f"{context}.faces")
        faces: list[tuple[str, ResolvedFace]] = []
        for face_name in FACE_VECTORS:
            if face_name not in raw_faces:
                continue
            face = _parse_model_face(
                raw_faces[face_name], source, cuboid_index, face_name
            )
            direction = VECTOR_FACES[FACE_VECTORS[face_name]]
            normal_axis = next(
                index
                for index, component in enumerate(FACE_VECTORS[face_name])
                if component
            )
            for axis in range(3):
                if axis != normal_axis and bounds[axis] == bounds[axis + 3]:
                    raise AssetImportError(
                        f"{context}.faces.{face_name} has zero area"
                    )
            faces.append((direction, face))
        unknown_faces = sorted(set(raw_faces) - set(FACE_VECTORS))
        if unknown_faces:
            raise AssetImportError(
                f"{context}.faces has unsupported faces: {', '.join(unknown_faces)}"
            )
        if not faces and any(
            bounds[axis] == bounds[axis + 3] for axis in range(3)
        ):
            raise AssetImportError(
                f"{context}: zero-thickness cuboid has no visible face"
            )
        cuboids.append(ResolvedCuboid(bounds, tuple(faces)))
    identifier, output_path = _normalized_geometry_location(source)
    return ResolvedGeometry(
        identifier, output_path, source, tuple(cuboids), len(raw_planes)
    )


def _is_builtin_full_cube(geometry: ResolvedGeometry) -> bool:
    if geometry.plane_count or len(geometry.cuboids) != 1:
        return False
    cuboid = geometry.cuboids[0]
    if cuboid.bounds != (0.0, 0.0, 0.0, 1.0, 1.0, 1.0):
        return False
    faces = dict(cuboid.faces)
    if set(faces) != set(VECTOR_FACES.values()):
        return False
    return all(
        face.uv == (0.0, 0.0, 1.0, 1.0)
        and face.rotation == 0
        and face.ambient_occlusion
        and face.cull_face
        and (face.shading_face is None or face.shading_face == face_name)
        for face_name, face in faces.items()
    )


class BlockModelResolver:
    def __init__(
        self,
        archive: zipfile.ZipFile,
        entries: dict[str, zipfile.ZipInfo],
    ) -> None:
        self.archive = archive
        self.entries = entries
        self.cache: dict[str, ResolvedModel] = {}
        self.texture_alpha_cache: dict[str, TextureAlphaClass] = {}
        self.active: set[str] = set()

    def texture_alpha_class(
        self, reference: str, context: str
    ) -> TextureAlphaClass:
        source = f"base/{reference}"
        alpha_class = self.texture_alpha_cache.get(source)
        if alpha_class is not None:
            return alpha_class
        info = self.entries.get(source)
        if info is None:
            raise AssetImportError(
                f"{context}: missing block texture {reference}"
            )
        alpha_class = classify_png_alpha(self.archive.read(info), source)
        self.texture_alpha_cache[source] = alpha_class
        return alpha_class

    def resolve(self, reference: str) -> ResolvedModel:
        source = _resource_archive_path(reference, "block model")
        if source in self.cache:
            return self.cache[source]
        if source in self.active:
            raise AssetImportError(f"block model parent cycle at {source}")
        info = self.entries.get(source)
        if info is None:
            raise AssetImportError(f"missing block model: {source}")
        self.active.add(source)
        try:
            document = _expect_object(
                load_relaxed_json(self.archive.read(info), source), source
            )
            _reject_unknown_keys(document, MODEL_ROOT_KEYS, source)
            parent = ResolvedModel({}, {}, False, False, True, False)
            if "parent" in document:
                parent_reference = document["parent"]
                if not isinstance(parent_reference, str):
                    raise AssetImportError(f"{source}.parent must be a string")
                parent = self.resolve(parent_reference)

            textures = dict(parent.textures)
            texture_entries = _expect_object(
                document.get("textures", {}), f"{source}.textures"
            )
            for alias, raw_entry in texture_entries.items():
                entry = _expect_object(raw_entry, f"{source}.textures.{alias}")
                _reject_unknown_keys(
                    entry, MODEL_TEXTURE_KEYS, f"{source}.textures.{alias}"
                )
                file_name = entry.get("fileName")
                if file_name is not None:
                    if not isinstance(file_name, str):
                        raise AssetImportError(f"{source}.textures.{alias}.fileName must be a string")
                    textures[alias] = _strip_base_namespace(file_name, source)
                emissive = entry.get("emissivefileName")
                if emissive is not None and not isinstance(emissive, str):
                    raise AssetImportError(
                        f"{source}.textures.{alias}.emissivefileName must be a string"
                    )
                raw_texture_uv = entry.get("uv")
                if raw_texture_uv is not None:
                    if (
                        not isinstance(raw_texture_uv, list)
                        or len(raw_texture_uv) != 2
                    ):
                        raise AssetImportError(
                            f"{source}.textures.{alias}.uv must contain two coordinates"
                        )
                    for index, value in enumerate(raw_texture_uv):
                        _finite_number(
                            value, f"{source}.textures.{alias}.uv[{index}]"
                        )

            geometry = parent.geometry
            if "cuboids" in document or "planes" in document:
                geometry = _parse_model_geometry(document, source)
            face_aliases: dict[str, str] = {}
            if geometry is not None and len(geometry.cuboids) == 1:
                face_aliases = {
                    face_name: face.texture_alias
                    for face_name, face in geometry.cuboids[0].faces
                }
            empty = geometry is None or (
                not geometry.cuboids and geometry.plane_count == 0
            )
            full_cube = geometry is not None and _is_builtin_full_cube(geometry)

            for shader_key in ("vertShader", "fragShader"):
                if (
                    shader_key in document
                    and not isinstance(document[shader_key], str)
                ):
                    raise AssetImportError(f"{source}.{shader_key} must be a string")

            transparent = document.get("isTransparent", parent.transparent)
            culls_self = document.get("cullsSelf", parent.culls_self)
            if not isinstance(transparent, bool) or not isinstance(culls_self, bool):
                raise AssetImportError(f"{source}: transparency/culling values must be booleans")
            result = ResolvedModel(
                textures, face_aliases, transparent, culls_self,
                empty, full_cube, geometry
            )
            self.cache[source] = result
            return result
        finally:
            self.active.remove(source)


@dataclass(frozen=True)
class GeneratorLeaf:
    parameters: dict[str, str]
    overrides: dict[str, object]
    model_name: str | None


class StateGeneratorResolver:
    def __init__(self, archive: zipfile.ZipFile, entries: dict[str, zipfile.ZipInfo]) -> None:
        self.definitions: dict[str, dict[str, object]] = {}
        self.cache: dict[str, list[GeneratorLeaf]] = {}
        self.active: set[str] = set()
        for source in sorted(entries):
            if not source.startswith("base/block_state_generators/") or not source.endswith(".json"):
                continue
            document = _expect_object(
                load_relaxed_json(archive.read(entries[source]), source), source
            )
            raw_generators = document.get("generators")
            if set(document) != {"generators"} or not isinstance(raw_generators, list):
                raise AssetImportError(f"{source}: expected one generators array")
            for index, raw_generator in enumerate(raw_generators):
                generator = _expect_object(raw_generator, f"{source}.generators[{index}]")
                _reject_unknown_keys(generator, GENERATOR_KEYS, f"{source}.generators[{index}]")
                identifier = generator.get("stringId")
                if not isinstance(identifier, str) or not identifier.startswith("base:"):
                    raise AssetImportError(f"{source}.generators[{index}]: invalid stringId")
                if identifier in self.definitions:
                    raise AssetImportError(f"duplicate state generator ID: {identifier}")
                self.definitions[identifier] = generator

    def resolve(self, identifier: str) -> list[GeneratorLeaf]:
        if identifier in self.cache:
            return self.cache[identifier]
        if identifier in self.active:
            raise AssetImportError(f"state generator include cycle at {identifier}")
        generator = self.definitions.get(identifier)
        if generator is None:
            raise AssetImportError(f"missing state generator: {identifier}")
        self.active.add(identifier)
        try:
            includes = generator.get("include", [])
            if not isinstance(includes, list) or not all(isinstance(item, str) for item in includes):
                raise AssetImportError(f"state generator {identifier}: include must be a string array")
            leaves: list[GeneratorLeaf] = []
            for included in includes:
                leaves.extend(self.resolve(included))
            has_leaf_data = any(key in generator for key in ("params", "overrides", "modelName"))
            if has_leaf_data:
                parameters = _parse_parameters(
                    generator.get("params", {}), f"state generator {identifier}.params"
                )
                overrides = _expect_object(
                    generator.get("overrides", {}), f"state generator {identifier}.overrides"
                )
                _reject_unknown_keys(
                    overrides, GENERATOR_OVERRIDE_KEYS, f"state generator {identifier}.overrides"
                )
                model_name = generator.get("modelName")
                if model_name is not None and not isinstance(model_name, str):
                    raise AssetImportError(f"state generator {identifier}.modelName must be a string")
                leaves.append(GeneratorLeaf(parameters, overrides, model_name))
            if not leaves:
                raise AssetImportError(f"state generator {identifier} resolves to no states")
            self.cache[identifier] = leaves
            return leaves
        finally:
            self.active.remove(identifier)


def _validate_block_properties(properties: dict[str, object], context: str) -> None:
    _reject_unknown_keys(properties, BLOCK_PROPERTY_KEYS, context)
    for key in ("isOpaque", "walkThrough"):
        if key in properties and not isinstance(properties[key], bool):
            raise AssetImportError(f"{context}.{key} must be a boolean")
    for key in ("lightAttenuation", "lightLevelRed", "lightLevelGreen", "lightLevelBlue"):
        if key in properties:
            value = properties[key]
            if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 15:
                raise AssetImportError(f"{context}.{key} must be an integer from 0 to 15")
    if "refractiveIndex" in properties:
        refractive_index = properties["refractiveIndex"]
        if (
            not isinstance(refractive_index, (int, float))
            or isinstance(refractive_index, bool)
            or not math.isfinite(refractive_index)
        ):
            raise AssetImportError(
                f"{context}.refractiveIndex must be a finite number"
            )
    if "modelName" in properties and not isinstance(properties["modelName"], str):
        raise AssetImportError(f"{context}.modelName must be a string")
    orientation = _block_orientation(properties.get("rotation"), f"{context}.rotation")
    rotate_top_bottom = properties.get("rotateTopBottom", False)
    if not isinstance(rotate_top_bottom, bool):
        raise AssetImportError(f"{context}.rotateTopBottom must be a boolean")
    if rotate_top_bottom and orientation not in ((90, 0, 0), (0, 0, 90)):
        raise AssetImportError(
            f"{context}.rotateTopBottom requires X90 or Z90 rotation"
        )


def _texture_from_alias(model: ResolvedModel, alias: str, context: str) -> str:
    if alias.startswith("base:"):
        return _strip_base_namespace(alias, context)
    if alias in model.textures:
        return model.textures[alias]
    for suffix, fallback_alias in (
        ("_side", "side"),
        ("_top", "top"),
        ("_bottom", "bottom"),
    ):
        if alias.endswith(suffix) and fallback_alias in model.textures:
            return model.textures[fallback_alias]
    if "all" in model.textures:
        return model.textures["all"]
    if alias.endswith(".png"):
        return alias if alias.startswith("textures/") else f"textures/blocks/{alias}"
    raise AssetImportError(f"{context}: model texture alias {alias!r} is unresolved")


def _resolved_face_textures(
    model: ResolvedModel, context: str
) -> dict[str, str]:
    result: dict[str, str] = {}
    for source_face, alias in model.face_aliases.items():
        destination_face = (
            source_face
            if source_face in VECTOR_FACES.values()
            else VECTOR_FACES[FACE_VECTORS[source_face]]
        )
        texture = _texture_from_alias(model, alias, context)
        previous = result.get(destination_face)
        if previous is not None and previous != texture:
            raise AssetImportError(
                f"{context}: representative model has conflicting {destination_face} textures"
            )
        result[destination_face] = texture
    if not result and model.textures:
        if "all" in model.textures:
            return {face: model.textures["all"] for face in VECTOR_FACES.values()}
        first = next(iter(model.textures.values()))
        return {face: first for face in VECTOR_FACES.values()}
    if result and set(result) != set(VECTOR_FACES.values()):
        fallback = model.textures.get("all") or next(iter(result.values()), None)
        if fallback is None:
            raise AssetImportError(f"{context}: model has no usable face texture")
        for face in VECTOR_FACES.values():
            result.setdefault(face, fallback)
    return result


def _yaml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def _yaml_mapping_key(value: str) -> str:
    if value and all(
        character.isalnum() or character in "_-" for character in value
    ):
        return value
    return _yaml_string(value)


def render_model_yaml(geometry: ResolvedGeometry) -> bytes:
    cuboids: list[dict[str, object]] = []
    for cuboid in geometry.cuboids:
        faces: dict[str, object] = {}
        for face_name, face in cuboid.faces:
            normalized: dict[str, object] = {
                "texture": face.texture_alias,
                "uv": list(face.uv),
                "rotation": face.rotation,
                "ambient_occlusion": face.ambient_occlusion,
                "cull": face.cull_face,
            }
            if face.shading_face is not None:
                normalized["shading"] = face.shading_face
            faces[face_name] = normalized
        cuboids.append({"bounds": list(cuboid.bounds), "faces": faces})
    return deterministic_json({
        "id": geometry.identifier,
        "texture_slots": list(geometry.texture_aliases()),
        "cuboids": cuboids,
    })


def _resolved_model_textures(
    model: ResolvedModel, context: str
) -> dict[str, str]:
    if model.geometry is None:
        return {}
    return {
        alias: _texture_from_alias(model, alias, context)
        for alias in model.geometry.texture_aliases()
    }


def render_block_yaml(
    identifier: str,
    properties: dict[str, object],
    models: BlockModelResolver,
    context: str,
    texture_model_reference: str | None = None,
    collision_audit: BlockCollisionImportAudit | None = None,
) -> bytes:
    _validate_block_properties(properties, context)
    model_reference = properties.get("modelName")
    if not isinstance(model_reference, str):
        raise AssetImportError(f"{context}: block state has no modelName")
    model = models.resolve(model_reference)
    if texture_model_reference is not None and texture_model_reference != model_reference:
        texture_model = models.resolve(texture_model_reference)
        textures = dict(model.textures)
        textures.update(texture_model.textures)
        model = ResolvedModel(
            textures,
            model.face_aliases,
            model.transparent or texture_model.transparent,
            model.culls_self or texture_model.culls_self,
            model.empty,
            model.full_cube,
            model.geometry,
        )
    opaque = properties.get("isOpaque", not model.transparent)
    solid = not properties.get("walkThrough", False)
    if not isinstance(opaque, bool) or not isinstance(solid, bool):
        raise AssetImportError(f"{context}: opacity/solidity values are invalid")
    orientation = _block_orientation(properties.get("rotation"), f"{context}.rotation")
    rotate_top_bottom = bool(properties.get("rotateTopBottom", False))
    collision = _resolved_collision_shape(model, properties, context)
    if collision_audit is not None:
        collision_audit.record(identifier, collision)
    texture_bindings = (
        _resolved_face_textures(model, context)
        if model.full_cube
        else _resolved_model_textures(model, context)
    )
    model_identifier = (
        "none" if model.empty
        else "cube" if model.full_cube
        else model.geometry.identifier if model.geometry is not None
        else "cube"
    )
    lines = [
        f"id: {_yaml_string(identifier)}",
        f"model: {model_identifier}",
        f"opaque: {'true' if opaque else 'false'}",
        f"solid: {'true' if solid else 'false'}",
        f"collision: {_render_collision_shape(collision)}",
        "collision_provenance: exact",
    ]
    if orientation != (0, 0, 0):
        lines.append(
            f"orientation: [{orientation[0]}, {orientation[1]}, {orientation[2]}]"
        )
    if rotate_top_bottom:
        lines.append("rotate_top_bottom: true")
    if model.culls_self or properties.get("isFluid", False):
        lines.append("cull_same_type: true")
    alpha_classes = {
        slot: models.texture_alpha_class(texture, context)
        for slot, texture in sorted(texture_bindings.items())
    }
    refractive = "refractiveIndex" in properties
    slot_layers = {
        slot: (
            "transparent"
            if alpha_class == TextureAlphaClass.FRACTIONAL
            or (refractive and alpha_class == TextureAlphaClass.BINARY)
            else "cutout"
            if alpha_class == TextureAlphaClass.BINARY
            else "opaque"
        )
        for slot, alpha_class in alpha_classes.items()
    }
    texture_render_layers: dict[str, str] = {}
    effective_layers = set(slot_layers.values())
    if model.empty or not effective_layers:
        layer = "opaque"
    elif len(effective_layers) == 1:
        layer = next(iter(effective_layers))
    elif model.full_cube:
        raise AssetImportError(
            f"{context}: full-cube texture slots require conflicting render layers"
        )
    else:
        layer_priority = {"opaque": 0, "cutout": 1, "transparent": 2}
        layer = min(
            effective_layers,
            key=lambda candidate: (
                -sum(value == candidate for value in slot_layers.values()),
                layer_priority[candidate],
            ),
        )
        texture_render_layers = {
            slot: slot_layer
            for slot, slot_layer in slot_layers.items()
            if slot_layer != layer
        }
    lines.append(f"layer: {layer}")
    if texture_render_layers:
        lines.append("texture_render_layers:")
        for slot, render_layer in texture_render_layers.items():
            lines.append(
                f"  {_yaml_mapping_key(slot)}: {_yaml_string(render_layer)}"
            )
    emitted = max(
        int(properties.get("lightLevelRed", 0)),
        int(properties.get("lightLevelGreen", 0)),
        int(properties.get("lightLevelBlue", 0)),
    )
    if emitted:
        lines.append(f"emits_light: {emitted}")
    attenuation = int(properties.get("lightAttenuation", 15))
    if attenuation != 15:
        lines.append(f"light_attenuation: {attenuation}")
    if texture_bindings:
        lines.append("textures:")
        values = set(texture_bindings.values())
        if model.full_cube and len(values) == 1:
            lines.append(f"  all: {_yaml_string(next(iter(values)))}")
        elif model.full_cube and (
            texture_bindings["pos_x"] == texture_bindings["neg_x"]
            == texture_bindings["pos_z"] == texture_bindings["neg_z"]
        ):
            lines.append(f"  top: {_yaml_string(texture_bindings['pos_y'])}")
            lines.append(f"  bottom: {_yaml_string(texture_bindings['neg_y'])}")
            lines.append(f"  sides: {_yaml_string(texture_bindings['pos_x'])}")
        else:
            for slot, texture in texture_bindings.items():
                lines.append(
                    f"  {_yaml_mapping_key(slot)}: {_yaml_string(texture)}"
                )
    return ("\n".join(lines) + "\n").encode("utf-8")


def compile_blocks(
    archive: zipfile.ZipFile,
    entries: dict[str, zipfile.ZipInfo],
    staging: Path,
    diagnostics: list[str] | None = None,
    model_audit: BlockModelImportAudit | None = None,
    collision_audit: BlockCollisionImportAudit | None = None,
) -> int:
    models = BlockModelResolver(archive, entries)
    generators = StateGeneratorResolver(archive, entries)
    generated: dict[str, tuple[bytes, str]] = {}
    generated_paths: dict[str, tuple[str, str]] = {}
    used_geometries: dict[str, ResolvedGeometry] = {}
    omitted_plane_states: list[tuple[str, str]] = []
    for source in sorted(entries):
        if not source.startswith("base/blocks/") or not source.endswith(".json"):
            continue
        document = _expect_object(load_relaxed_json(archive.read(entries[source]), source), source)
        _reject_unknown_keys(document, BLOCK_ROOT_KEYS, source)
        base_identifier = document.get("stringId")
        if not isinstance(base_identifier, str) or ":" not in base_identifier:
            raise AssetImportError(f"{source}: invalid stringId")
        defaults = _expect_object(document.get("defaultProperties", {}), f"{source}.defaultProperties")
        _validate_block_properties(defaults, f"{source}.defaultProperties")
        default_parameters = _parse_parameters(
            document.get("defaultParams", {}), f"{source}.defaultParams"
        )
        states = _expect_object(document.get("blockStates"), f"{source}.blockStates")
        if not states:
            raise AssetImportError(f"{source}: blockStates must not be empty")
        for state_name, raw_state in states.items():
            state = _expect_object(raw_state, f"{source}.blockStates.{state_name}")
            _validate_block_properties(state, f"{source}.blockStates.{state_name}")
            parameters = (
                dict(default_parameters)
                if state_name == "default"
                else _parameters_from_state_name(state_name, source)
            )
            properties = dict(defaults)
            properties.update(state)
            generator_ids = properties.pop("stateGenerators", [])
            if isinstance(generator_ids, str):
                generator_ids = [generator_ids]
            if not isinstance(generator_ids, list) or not all(
                isinstance(item, str) for item in generator_ids
            ):
                raise AssetImportError(f"{source}.{state_name}: stateGenerators must be strings")

            variants: list[tuple[dict[str, str], dict[str, object], str | None]] = [
                (parameters, properties, None)
            ]
            base_model_name = properties.get("modelName")
            if not isinstance(base_model_name, str):
                raise AssetImportError(f"{source}.{state_name}: block state has no modelName")
            for generator_id in generator_ids:
                for leaf in generators.resolve(generator_id):
                    generated_parameters = dict(parameters)
                    generated_parameters.update(leaf.parameters)
                    generated_properties = dict(properties)
                    generated_properties.update(leaf.overrides)
                    if leaf.model_name is not None:
                        generated_properties["modelName"] = leaf.model_name
                    variants.append(
                        (generated_parameters, generated_properties, base_model_name)
                    )

            for variant_parameters, variant_properties, texture_model in variants:
                identifiers = [_block_identifier(base_identifier, variant_parameters)]
                if LEGACY_DEFAULT_STATE_ALIASES.get(base_identifier) == variant_parameters:
                    identifiers.append(base_identifier)
                model_reference = variant_properties.get("modelName")
                if not isinstance(model_reference, str):
                    raise AssetImportError(
                        f"{source}: block state has no modelName"
                    )
                resolved_model = models.resolve(model_reference)
                has_planes = (
                    resolved_model.geometry is not None
                    and resolved_model.geometry.plane_count != 0
                )
                if model_audit is not None:
                    model_audit.record_geometry(
                        identifiers,
                        generated_candidate=texture_model is not None,
                        requires_cuboid_model=_requires_geometric_cuboid_model(
                            resolved_model
                        ),
                        has_planes=has_planes,
                    )
                if has_planes:
                    omitted_plane_states.extend(
                        (identifier, resolved_model.geometry.source)
                        for identifier in identifiers
                    )
                    continue
                if (
                    not resolved_model.empty
                    and not resolved_model.full_cube
                    and resolved_model.geometry is not None
                ):
                    previous = used_geometries.setdefault(
                        resolved_model.geometry.identifier,
                        resolved_model.geometry,
                    )
                    if previous != resolved_model.geometry:
                        raise AssetImportError(
                            "conflicting normalized block model identifier: "
                            + resolved_model.geometry.identifier
                        )
                for identifier in identifiers:
                    output = _generated_block_filename(identifier)
                    if identifier in generated:
                        previous_source = generated[identifier][1]
                        raise AssetImportError(
                            f"duplicate generated block identifier {identifier}: "
                            f"{previous_source} and {source}"
                        )
                    previous_output = generated_paths.get(output)
                    if previous_output is not None:
                        raise AssetImportError(
                            f"duplicate generated logical path {output}: "
                            f"{previous_output[0]} from {previous_output[1]} and "
                            f"{identifier} from {source}"
                        )
                    payload = render_block_yaml(
                        identifier,
                        variant_properties,
                        models,
                        source,
                        texture_model,
                        collision_audit,
                    )
                    generated[identifier] = (payload, source)
                    generated_paths[output] = (identifier, source)
                    write_output(staging, output, payload)
    for geometry in sorted(
        used_geometries.values(), key=lambda value: value.output_path
    ):
        write_output(staging, geometry.output_path, render_model_yaml(geometry))
    if omitted_plane_states and diagnostics is not None:
        sources = sorted({
            source for unused_identifier, source in omitted_plane_states
        })
        diagnostics.append(
            f"omitted {len(omitted_plane_states)} block states with explicit "
            f"plane geometry (unsupported): {', '.join(sources)}"
        )
    return len(generated)


def _parse_generated_collision(
    raw_value: str, source: str, line_number: int
) -> str | dict[str, object]:
    context = f"{source}:{line_number}: collision"
    if raw_value in ("none", "full"):
        return raw_value
    try:
        decoded = json.loads(raw_value)
    except json.JSONDecodeError as error:
        raise AssetImportError(
            f"{context} must be none, full, or a boxes mapping"
        ) from error
    mapping = _expect_object(decoded, f"{source}.collision")
    if set(mapping) != {"boxes"}:
        raise AssetImportError(
            f"{source}.collision must contain exactly one boxes field"
        )
    raw_boxes = mapping["boxes"]
    if not isinstance(raw_boxes, list) or not raw_boxes:
        raise AssetImportError(
            f"{source}.collision.boxes must be a non-empty array"
        )
    boxes: list[list[float]] = []
    for box_index, raw_box in enumerate(raw_boxes):
        box_context = f"{source}.collision.boxes[{box_index}]"
        if not isinstance(raw_box, list) or len(raw_box) != 6:
            raise AssetImportError(
                f"{box_context} must contain six coordinates"
            )
        box = [
            _finite_number(value, f"{box_context}[{index}]")
            for index, value in enumerate(raw_box)
        ]
        for axis in range(3):
            if box[axis] >= box[axis + 3]:
                raise AssetImportError(
                    f"{box_context} must have positive volume"
                )
        if any(
            coordinate < BLOCK_COLLISION_MINIMUM_COORDINATE
            or coordinate > BLOCK_COLLISION_MAXIMUM_COORDINATE
            for coordinate in box
        ):
            raise AssetImportError(
                f"{box_context} exceeds the supported "
                f"[{BLOCK_COLLISION_MINIMUM_COORDINATE}, "
                f"{BLOCK_COLLISION_MAXIMUM_COORDINATE}] range"
            )
        if box in boxes:
            raise AssetImportError(
                f"{source}.collision.boxes must not contain duplicates"
            )
        boxes.append(box)
    if boxes == [[0.0, 0.0, 0.0, 1.0, 1.0, 1.0]]:
        raise AssetImportError(
            f"{source}.collision canonical unit geometry must use full"
        )
    return {"boxes": boxes}


def parse_generated_block(data: bytes, source: str) -> dict[str, object]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise AssetImportError(f"{source}: generated block YAML is not UTF-8") from error
    result: dict[str, object] = {}
    mappings: dict[str, dict[str, str]] = {
        "textures": {},
        "texture_render_layers": {},
    }
    active_mapping: str | None = None
    for line_number, line in enumerate(lines, start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith("  "):
            if active_mapping is None or ":" not in line:
                raise AssetImportError(f"{source}:{line_number}: malformed generated YAML")
            stripped = line.strip()
            try:
                if stripped.startswith('"'):
                    entry = json.loads("{" + stripped + "}")
                    if not isinstance(entry, dict) or len(entry) != 1:
                        raise json.JSONDecodeError("invalid entry", stripped, 0)
                    key, value = next(iter(entry.items()))
                else:
                    key, raw_value = stripped.split(":", 1)
                    value = json.loads(raw_value.strip())
            except json.JSONDecodeError as error:
                raise AssetImportError(
                    f"{source}:{line_number}: malformed mapping entry"
                ) from error
            mapping = mappings[active_mapping]
            if (
                not isinstance(key, str)
                or not key
                or key in mapping
                or not isinstance(value, str)
            ):
                raise AssetImportError(f"{source}:{line_number}: invalid mapping entry")
            mapping[key] = value
            continue
        active_mapping = None
        if ":" not in line:
            raise AssetImportError(f"{source}:{line_number}: malformed generated YAML")
        key, raw_value = line.split(":", 1)
        if key in result:
            raise AssetImportError(f"{source}:{line_number}: duplicate field {key}")
        value = raw_value.strip()
        if key in mappings:
            if value:
                raise AssetImportError(f"{source}:{line_number}: {key} must be a map")
            active_mapping = key
            result[key] = mappings[key]
        elif key == "id":
            try:
                decoded = json.loads(value)
            except json.JSONDecodeError as error:
                raise AssetImportError(f"{source}:{line_number}: id must be quoted") from error
            if not isinstance(decoded, str):
                raise AssetImportError(f"{source}:{line_number}: id must be a string")
            result[key] = decoded
        elif key in ("opaque", "solid", "cull_same_type", "rotate_top_bottom"):
            if value not in ("true", "false"):
                raise AssetImportError(f"{source}:{line_number}: {key} must be boolean")
            result[key] = value == "true"
        elif key == "orientation":
            try:
                decoded = json.loads(value)
            except json.JSONDecodeError as error:
                raise AssetImportError(
                    f"{source}:{line_number}: orientation must be an angle array"
                ) from error
            result[key] = list(
                _block_orientation(decoded, f"{source}.orientation")
            )
        elif key == "collision":
            result[key] = _parse_generated_collision(
                value, source, line_number
            )
        elif key == "collision_provenance":
            if value not in ("exact", "conservative_fallback"):
                raise AssetImportError(
                    f"{source}:{line_number}: unsupported collision provenance"
                )
            result[key] = value
        elif key in ("emits_light", "light_attenuation"):
            try:
                result[key] = int(value)
            except ValueError as error:
                raise AssetImportError(f"{source}:{line_number}: {key} must be integer") from error
        else:
            result[key] = value
    allowed = {
        "id", "model", "opaque", "solid", "cull_same_type", "layer",
        "emits_light", "light_attenuation", "orientation",
        "rotate_top_bottom", "collision", "textures",
        "collision_provenance", "texture_render_layers",
    }
    _reject_unknown_keys(result, allowed, source)
    required = {
        "id", "model", "opaque", "solid", "collision",
        "collision_provenance", "layer",
    }
    missing = sorted(required - set(result))
    if missing:
        raise AssetImportError(f"{source}: missing generated fields: {', '.join(missing)}")
    if not isinstance(result["model"], str) or not result["model"]:
        raise AssetImportError(f"{source}: invalid model {result['model']!r}")
    if result["layer"] not in ("opaque", "cutout", "transparent", "emissive"):
        raise AssetImportError(f"{source}: invalid render layer {result['layer']!r}")
    texture_render_layers = mappings["texture_render_layers"]
    for slot, render_layer in texture_render_layers.items():
        if slot not in mappings["textures"]:
            raise AssetImportError(
                f"{source}: render-layer slot {slot!r} has no texture binding"
            )
        if render_layer not in ("opaque", "cutout", "transparent", "emissive"):
            raise AssetImportError(
                f"{source}: invalid texture render layer {render_layer!r}"
            )
    orientation = tuple(result.get("orientation", (0, 0, 0)))
    if result.get("rotate_top_bottom", False) and orientation not in (
        (90, 0, 0), (0, 0, 90)
    ):
        raise AssetImportError(
            f"{source}: rotate_top_bottom requires X90 or Z90 orientation"
        )
    for key in ("emits_light", "light_attenuation"):
        if key in result and not 0 <= int(result[key]) <= 15:
            raise AssetImportError(f"{source}: {key} is outside 0..15")
    textures = mappings["textures"]
    if texture_render_layers and result["model"] in ("cube", "none"):
        raise AssetImportError(
            f"{source}: texture render layers require a normalized model"
        )
    if textures and result["model"] == "cube":
        allowed_texture_keys = {
            "all", "top", "bottom", "sides", "default", "pos_x", "neg_x",
            "pos_y", "neg_y", "pos_z", "neg_z",
        }
        _reject_unknown_keys(textures, allowed_texture_keys, f"{source}.textures")
        pattern_valid = (
            set(textures) == {"all"}
            or set(textures) == {"top", "bottom", "sides"}
            or set(textures)
            == {"pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z"}
        )
        if not pattern_valid:
            raise AssetImportError(f"{source}: generated texture mapping is incomplete")
    elif result["model"] == "cube":
        raise AssetImportError(f"{source}: cube model has no textures")
    elif result["model"] == "none" and textures:
        raise AssetImportError(f"{source}: empty model must not bind textures")
    return result


def parse_generated_model(data: bytes, source: str) -> dict[str, object]:
    document = _expect_object(load_relaxed_json(data, source), source)
    _reject_unknown_keys(document, {"id", "texture_slots", "cuboids"}, source)
    missing = {"id", "texture_slots", "cuboids"} - set(document)
    if missing:
        raise AssetImportError(
            f"{source}: missing normalized model fields: {', '.join(sorted(missing))}"
        )
    identifier = document["id"]
    if not isinstance(identifier, str) or not identifier:
        raise AssetImportError(f"{source}.id must be a non-empty string")
    slots = document["texture_slots"]
    if (
        not isinstance(slots, list)
        or not all(isinstance(slot, str) and slot for slot in slots)
        or len(set(slots)) != len(slots)
    ):
        raise AssetImportError(
            f"{source}.texture_slots must contain unique non-empty strings"
        )
    cuboids = document["cuboids"]
    if not isinstance(cuboids, list) or not cuboids:
        raise AssetImportError(f"{source}.cuboids must be a non-empty array")
    for cuboid_index, raw_cuboid in enumerate(cuboids):
        context = f"{source}.cuboids[{cuboid_index}]"
        cuboid = _expect_object(raw_cuboid, context)
        _reject_unknown_keys(cuboid, {"bounds", "faces"}, context)
        if set(cuboid) != {"bounds", "faces"}:
            raise AssetImportError(f"{context} requires bounds and faces")
        bounds = cuboid["bounds"]
        if not isinstance(bounds, list) or len(bounds) != 6:
            raise AssetImportError(f"{context}.bounds must contain six coordinates")
        numeric_bounds = tuple(
            _finite_number(value, f"{context}.bounds[{index}]")
            for index, value in enumerate(bounds)
        )
        for axis in range(3):
            if numeric_bounds[axis] > numeric_bounds[axis + 3]:
                raise AssetImportError(
                    f"{context}.bounds minimum exceeds maximum"
                )
        faces = _expect_object(cuboid["faces"], f"{context}.faces")
        for face_name, raw_face in faces.items():
            if face_name not in VECTOR_FACES.values():
                raise AssetImportError(
                    f"{context}.faces.{face_name}: invalid cardinal face"
                )
            face_context = f"{context}.faces.{face_name}"
            face = _expect_object(raw_face, face_context)
            allowed = {
                "texture", "uv", "rotation", "shading",
                "ambient_occlusion", "cull",
            }
            _reject_unknown_keys(face, allowed, face_context)
            required = allowed - {"shading"}
            if not required.issubset(face):
                raise AssetImportError(f"{face_context}: missing face metadata")
            if (
                not isinstance(face["texture"], str)
                or face["texture"] not in slots
            ):
                raise AssetImportError(
                    f"{face_context}.texture is not a declared slot"
                )
            uv = face["uv"]
            if not isinstance(uv, list) or len(uv) != 4:
                raise AssetImportError(f"{face_context}.uv must contain four coordinates")
            numeric_uv = tuple(
                _finite_number(value, f"{face_context}.uv[{index}]")
                for index, value in enumerate(uv)
            )
            if any(value < 0.0 or value > 1.0 for value in numeric_uv):
                raise AssetImportError(f"{face_context}.uv is outside 0..1")
            rotation = face["rotation"]
            if (
                not isinstance(rotation, int)
                or isinstance(rotation, bool)
                or rotation not in (0, 90, 180, 270)
            ):
                raise AssetImportError(f"{face_context}.rotation is not a quarter turn")
            for key in ("ambient_occlusion", "cull"):
                if not isinstance(face[key], bool):
                    raise AssetImportError(f"{face_context}.{key} must be a boolean")
            if (
                "shading" in face
                and face["shading"] not in VECTOR_FACES.values()
            ):
                raise AssetImportError(
                    f"{face_context}.shading must be a cardinal face"
                )
            direction_index = tuple(VECTOR_FACES.values()).index(face_name)
            normal_axis = direction_index // 2
            for axis in range(3):
                if (
                    axis != normal_axis
                    and numeric_bounds[axis] == numeric_bounds[axis + 3]
                ):
                    raise AssetImportError(f"{face_context} has zero area")
        if not faces:
            raise AssetImportError(
                f"{context}.faces must contain at least one visible face"
            )
    return document


def _validate_generated_texture_reference(reference: object, context: str) -> str:
    if not isinstance(reference, str):
        raise AssetImportError(f"{context}: texture reference must be a string")
    try:
        normalized = normalize_archive_path(reference).as_posix()
    except AssetImportError as error:
        raise AssetImportError(
            f"{context}: invalid generated texture reference {reference!r}"
        ) from error
    if (
        normalized != reference
        or not reference.startswith("textures/")
        or not reference.endswith(".png")
    ):
        raise AssetImportError(
            f"{context}: invalid generated texture reference {reference!r}"
        )
    return reference


def prune_unused_block_models(root: Path) -> int:
    referenced: set[str] = set()
    for path in sorted((root / "blocks").glob("*.yaml")):
        block = parse_generated_block(
            path.read_bytes(), path.relative_to(root).as_posix()
        )
        if block["model"] not in ("cube", "none"):
            referenced.add(str(block["model"]))
    removed = 0
    model_root = root / "models/blocks"
    for path in sorted(model_root.rglob("*.yaml")) if model_root.is_dir() else []:
        logical = path.relative_to(root).as_posix()
        model = parse_generated_model(path.read_bytes(), logical)
        if model["id"] not in referenced:
            path.unlink()
            removed += 1
    return removed


def omit_blocks_with_unsupported_textures(
    root: Path,
    diagnostics: list[str] | None = None,
    model_audit: BlockModelImportAudit | None = None,
    collision_audit: BlockCollisionImportAudit | None = None,
) -> int:
    omitted: list[str] = []
    unsupported: set[str] = set()
    for path in sorted((root / "blocks").glob("*.yaml")):
        logical = path.relative_to(root).as_posix()
        block = parse_generated_block(path.read_bytes(), logical)
        textures = block.get("textures", {})
        if not isinstance(textures, dict):
            continue
        incompatible = False
        for reference in textures.values():
            texture = root / str(reference)
            if not texture.is_file():
                continue
            dimensions = png_dimensions(texture.read_bytes(), str(reference))
            if dimensions != BLOCK_TEXTURE_SIZE:
                incompatible = True
                unsupported.add(
                    f"{reference} ({dimensions[0]}x{dimensions[1]})"
                )
        if incompatible:
            omitted.append(str(block["id"]))
            path.unlink()

    if omitted and diagnostics is not None:
        diagnostics.append(
            f"omitted {len(omitted)} blocks whose textures are not "
            f"{BLOCK_TEXTURE_SIZE[0]}x{BLOCK_TEXTURE_SIZE[1]}: "
            + ", ".join(sorted(unsupported))
        )
    if model_audit is not None:
        model_audit.record_nonstandard_textures(omitted)
    if collision_audit is not None:
        collision_audit.discard(omitted)
    prune_unused_block_models(root)
    return len(omitted)


def validate_block_model_import_report(value: object) -> dict[str, object]:
    report = _expect_object(value, "block_model_import")
    required = {
        "schema",
        "candidate_states",
        "newly_recovered_states",
        "base_approximation_states",
        "corrected_approximations",
        "omissions",
    }
    if set(report) != required:
        raise AssetImportError(
            "block_model_import must contain the complete supported report"
        )

    def count(mapping: dict[str, object], key: str, context: str) -> int:
        result = mapping.get(key)
        if (
            not isinstance(result, int)
            or isinstance(result, bool)
            or result < 0
        ):
            raise AssetImportError(f"{context}.{key} must be a non-negative integer")
        return result

    schema = count(report, "schema", "block_model_import")
    if schema != BLOCK_MODEL_SUPPORT_SCHEMA:
        raise AssetImportError("block_model_import schema is unsupported")
    candidates = count(report, "candidate_states", "block_model_import")
    recovered = count(report, "newly_recovered_states", "block_model_import")
    base_states = count(
        report, "base_approximation_states", "block_model_import"
    )
    corrected = count(
        report, "corrected_approximations", "block_model_import"
    )
    if recovered > candidates or corrected > base_states:
        raise AssetImportError("block_model_import recovery counts are inconsistent")

    omissions = _expect_object(report["omissions"], "block_model_import.omissions")
    expected_reasons = {
        "plane_or_mixed_geometry",
        "nonstandard_texture_dimensions",
    }
    if set(omissions) != expected_reasons:
        raise AssetImportError(
            "block_model_import omissions must use the supported disjoint reasons"
        )
    omitted_candidates = 0
    omitted_base_states = 0
    omitted_identifiers: set[str] = set()
    for reason in sorted(expected_reasons):
        context = f"block_model_import.omissions.{reason}"
        summary = _expect_object(omissions[reason], context)
        if set(summary) != {
            "block_states",
            "candidate_states",
            "base_approximation_states",
            "other_states",
        }:
            raise AssetImportError(f"{context} has incomplete omission metadata")
        states = summary["block_states"]
        if (
            not isinstance(states, list)
            or not all(isinstance(state, str) and state for state in states)
            or states != sorted(set(states))
        ):
            raise AssetImportError(
                f"{context}.block_states must be sorted unique identifiers"
            )
        overlap = omitted_identifiers & set(states)
        if overlap:
            raise AssetImportError(
                "block_model_import omission reasons overlap: "
                + ", ".join(sorted(overlap))
            )
        omitted_identifiers.update(states)
        reason_candidates = count(summary, "candidate_states", context)
        reason_base_states = count(
            summary, "base_approximation_states", context
        )
        reason_other = count(summary, "other_states", context)
        if reason_candidates + reason_base_states + reason_other != len(states):
            raise AssetImportError(f"{context} omission counts are inconsistent")
        omitted_candidates += reason_candidates
        omitted_base_states += reason_base_states

    if candidates != recovered + omitted_candidates:
        raise AssetImportError(
            "block_model_import candidate recovery count is inconsistent"
        )
    if base_states != corrected + omitted_base_states:
        raise AssetImportError(
            "block_model_import approximation correction count is inconsistent"
        )
    return report


def audit_generated_collision_shapes(root: Path) -> dict[str, int]:
    counts = {
        "empty": 0,
        "full": 0,
        "single_partial": 0,
        "multi_box": 0,
        "exact_derived": 0,
        "conservative_fallback": 0,
        "ambiguous": 0,
    }
    for path in sorted((root / "blocks").glob("*.yaml")):
        logical = path.relative_to(root).as_posix()
        block = parse_generated_block(path.read_bytes(), logical)
        collision = block["collision"]
        if collision == "none":
            category = "empty"
        elif collision == "full":
            category = "full"
        else:
            assert isinstance(collision, dict)
            boxes = collision["boxes"]
            assert isinstance(boxes, list)
            category = "single_partial" if len(boxes) == 1 else "multi_box"
        counts[category] += 1
        provenance = block["collision_provenance"]
        if provenance == "exact":
            counts["exact_derived"] += 1
        else:
            assert provenance == "conservative_fallback"
            counts["conservative_fallback"] += 1
    return counts


def validate_block_collision_import_report(
    value: object,
    shape_counts: dict[str, int] | None = None,
) -> dict[str, object]:
    report = _expect_object(value, "block_collision_import")
    required = {
        "schema",
        "empty",
        "full",
        "single_partial",
        "multi_box",
        "exact_derived",
        "conservative_fallback",
        "ambiguous",
    }
    if set(report) != required:
        raise AssetImportError(
            "block_collision_import must contain the complete supported report"
        )

    counts: dict[str, int] = {}
    for key in sorted(required):
        raw_count = report[key]
        if (
            not isinstance(raw_count, int)
            or isinstance(raw_count, bool)
            or raw_count < 0
        ):
            raise AssetImportError(
                f"block_collision_import.{key} must be a non-negative integer"
            )
        counts[key] = raw_count
    if counts["schema"] != BLOCK_COLLISION_SUPPORT_SCHEMA:
        raise AssetImportError("block_collision_import schema is unsupported")

    registration_count = sum(
        counts[key]
        for key in ("empty", "full", "single_partial", "multi_box")
    )
    if (
        counts["exact_derived"] != registration_count
        or counts["conservative_fallback"] != 0
        or counts["ambiguous"] != 0
    ):
        raise AssetImportError(
            "block_collision_import published registrations must be exact "
            "derivations; fallback and ambiguity are unsupported"
        )
    if shape_counts is not None and any(
        counts[key] != shape_counts.get(key)
        for key in (
            "empty",
            "full",
            "single_partial",
            "multi_box",
            "exact_derived",
            "conservative_fallback",
            "ambiguous",
        )
    ):
        raise AssetImportError(
            "block_collision_import counts do not match the asset tree"
        )
    return report


def validate_generated_tree(
    root: Path,
    required_identifiers: tuple[str, ...] = REQUIRED_BLOCK_IDENTIFIERS,
) -> dict[str, int]:
    if not root.is_dir():
        raise AssetImportError(f"generated tree does not exist: {root}")
    files = sorted(path for path in root.rglob("*") if path.is_file())
    logical_paths: set[str] = set()
    for path in files:
        logical = path.relative_to(root).as_posix()
        normalize_archive_path(logical)
        if logical in logical_paths:
            raise AssetImportError(f"duplicate generated logical path: {logical}")
        logical_paths.add(logical)

    for logical in sorted(
        path for path in logical_paths
        if path.startswith("textures/") and path.endswith(".png")
    ):
        png_dimensions((root / logical).read_bytes(), logical)

    normalized_models: dict[str, tuple[str, set[str]]] = {}
    block_model_paths = sorted(
        path for path in logical_paths
        if path.startswith("models/blocks/") and path.endswith(".yaml")
    )
    for logical in block_model_paths:
        model = parse_generated_model((root / logical).read_bytes(), logical)
        identifier = str(model["id"])
        if identifier in ("cube", "none"):
            raise AssetImportError(
                f"{logical}: normalized block model identifier collides with "
                f"built-in model {identifier}"
            )
        if identifier in normalized_models:
            raise AssetImportError(
                f"duplicate normalized block model identifier {identifier}: "
                f"{normalized_models[identifier][0]} and {logical}"
            )
        normalized_models[identifier] = (
            logical, set(str(slot) for slot in model["texture_slots"])
        )

    block_identifiers: dict[str, str] = {}
    referenced_textures: dict[str, str] = {}
    block_texture_layers: list[tuple[str, str, str, str]] = []
    block_paths = sorted(
        path for path in logical_paths
        if path.startswith("blocks/") and path.endswith(".yaml")
    )
    for logical in block_paths:
        block = parse_generated_block((root / logical).read_bytes(), logical)
        identifier = str(block["id"])
        if identifier in block_identifiers:
            raise AssetImportError(
                f"duplicate generated block identifier {identifier}: "
                f"{block_identifiers[identifier]} and {logical}"
            )
        block_identifiers[identifier] = logical
        textures = block.get("textures", {})
        model_identifier = str(block["model"])
        if model_identifier not in ("cube", "none"):
            normalized = normalized_models.get(model_identifier)
            if normalized is None:
                raise AssetImportError(
                    f"{logical}: unresolved normalized block model {model_identifier}"
                )
            if not isinstance(textures, dict) or set(textures) != normalized[1]:
                raise AssetImportError(
                    f"{logical}: texture bindings do not match model {model_identifier}"
                )
        if isinstance(textures, dict):
            texture_layers = block.get("texture_render_layers", {})
            if not isinstance(texture_layers, dict):
                raise AssetImportError(
                    f"{logical}: texture_render_layers must be a map"
                )
            for slot, texture in textures.items():
                reference = _validate_generated_texture_reference(
                    texture, f"{logical}.textures.{slot}"
                )
                referenced_textures[reference] = logical
                block_texture_layers.append((
                    logical,
                    str(slot),
                    reference,
                    str(texture_layers.get(slot, block["layer"])),
                ))

    namespace_collisions = sorted(
        set(block_identifiers) & set(normalized_models)
    )
    if namespace_collisions:
        raise AssetImportError(
            "block/model identifier namespace collision: "
            + ", ".join(namespace_collisions)
        )

    missing_required = sorted(set(required_identifiers) - set(block_identifiers))
    if missing_required:
        raise AssetImportError(
            "generated block registry lacks required runtime materials: "
            + ", ".join(missing_required)
        )

    model_paths = sorted(
        path for path in logical_paths
        if path.startswith("models/entities/") and path.endswith(".json")
    )
    entity_model_identifiers: dict[str, str] = {}
    for logical in model_paths:
        model = _expect_object(
            load_relaxed_json((root / logical).read_bytes(), logical), logical
        )
        identifier = model.get("id")
        if not isinstance(identifier, str) or not identifier:
            raise AssetImportError(f"{logical}: entity model has no logical ID")
        previous_model = entity_model_identifiers.get(identifier)
        if previous_model is not None:
            raise AssetImportError(
                f"duplicate entity model identifier {identifier}: "
                f"{previous_model} and {logical}"
            )
        entity_model_identifiers[identifier] = logical
        textures = _expect_object(model.get("textures", {}), f"{logical}.textures")
        for slot, texture in textures.items():
            reference = _validate_generated_texture_reference(
                texture, f"{logical}.textures.{slot}"
            )
            referenced_textures[reference] = logical

    animation_paths = sorted(
        path for path in logical_paths
        if path.startswith("animations/entities/") and path.endswith(".json")
    )
    for logical in animation_paths:
        document = load_relaxed_json((root / logical).read_bytes(), logical)
        if not isinstance(document, dict) or not isinstance(document.get("animations"), dict):
            raise AssetImportError(f"{logical}: animation file has no animations object")

    missing_textures = sorted(
        f"{reference} (from {consumer})"
        for reference, consumer in referenced_textures.items()
        if reference not in logical_paths
    )
    if missing_textures:
        preview = "; ".join(missing_textures[:8])
        remainder = len(missing_textures) - min(len(missing_textures), 8)
        suffix = f"; and {remainder} more" if remainder else ""
        raise AssetImportError(f"generated texture references are unresolved: {preview}{suffix}")

    alpha_classes: dict[str, TextureAlphaClass] = {}
    coherent_layers = {
        TextureAlphaClass.FULLY_OPAQUE: {"opaque", "emissive"},
        TextureAlphaClass.BINARY: {"cutout", "transparent", "emissive"},
        TextureAlphaClass.FRACTIONAL: {"transparent", "emissive"},
    }
    for logical, slot, reference, render_layer in block_texture_layers:
        alpha_class = alpha_classes.get(reference)
        if alpha_class is None:
            alpha_class = classify_png_alpha(
                (root / reference).read_bytes(), reference
            )
            alpha_classes[reference] = alpha_class
        if render_layer not in coherent_layers[alpha_class]:
            raise AssetImportError(
                f"{logical}: texture slot {slot!r} uses {render_layer!r} "
                f"with {alpha_class.value} PNG alpha"
            )

    return {
        "blocks": len(block_identifiers),
        "textures": sum(
            path.startswith("textures/") and path.endswith(".png")
            for path in logical_paths
        ),
        "models": len(model_paths),
        "block_models": len(block_model_paths),
        "animations": len(animation_paths),
        "sounds": sum(
            path.startswith("sounds/") and path.endswith(".ogg")
            for path in logical_paths
        ),
    }


def audit_generated_material_layers(root: Path) -> dict[str, object]:
    alpha_counts = {
        alpha_class.value: 0
        for alpha_class in TextureAlphaClass
    }
    alpha_by_texture: dict[str, TextureAlphaClass] = {}
    texture_root = root / "textures"
    if texture_root.is_dir():
        for path in sorted(texture_root.rglob("*.png")):
            logical = path.relative_to(root).as_posix()
            alpha_class = classify_png_alpha(path.read_bytes(), logical)
            alpha_by_texture[logical] = alpha_class
            alpha_counts[alpha_class.value] += 1

    registration_counts = {
        "opaque_only": 0,
        "cutout_only": 0,
        "transparent_only": 0,
        "mixed": 0,
        "other_only": 0,
        "empty": 0,
    }
    canonical_layer = {
        TextureAlphaClass.FULLY_OPAQUE: "opaque",
        TextureAlphaClass.BINARY: "cutout",
        TextureAlphaClass.FRACTIONAL: "transparent",
    }
    suspicious: list[dict[str, str]] = []
    block_root = root / "blocks"
    if block_root.is_dir():
        for path in sorted(block_root.rglob("*.yaml")):
            logical = path.relative_to(root).as_posix()
            block = parse_generated_block(path.read_bytes(), logical)
            identifier = str(block["id"])
            textures = block.get("textures", {})
            texture_layers = block.get("texture_render_layers", {})
            if not isinstance(textures, dict) or not isinstance(texture_layers, dict):
                raise AssetImportError(
                    f"{logical}: generated texture mappings are invalid"
                )

            effective_layers: set[str] = set()
            for slot in sorted(textures):
                texture = _validate_generated_texture_reference(
                    textures[slot], f"{logical}.textures.{slot}"
                )
                alpha_class = alpha_by_texture.get(texture)
                if alpha_class is None:
                    raise AssetImportError(
                        f"{logical}: texture slot {slot!r} references an "
                        f"unaudited PNG: {texture}"
                    )
                render_layer = str(texture_layers.get(slot, block["layer"]))
                effective_layers.add(render_layer)
                if render_layer != canonical_layer[alpha_class]:
                    suspicious.append({
                        "identifier": identifier,
                        "slot": str(slot),
                        "texture": texture,
                        "alpha_class": alpha_class.value,
                        "render_layer": render_layer,
                    })

            if not effective_layers:
                registration_counts["empty"] += 1
            elif len(effective_layers) > 1:
                registration_counts["mixed"] += 1
            else:
                only_layer = next(iter(effective_layers))
                category = {
                    "opaque": "opaque_only",
                    "cutout": "cutout_only",
                    "transparent": "transparent_only",
                }.get(only_layer, "other_only")
                registration_counts[category] += 1

    return {
        "schema": MATERIAL_LAYER_AUDIT_SCHEMA,
        "texture_alpha_classes": alpha_counts,
        "effective_registration_layers": registration_counts,
        "suspicious_cross_classifications": suspicious,
    }


def validate_material_layer_audit_report(
    report: object,
    counts: object | None = None,
) -> None:
    if not isinstance(report, dict):
        raise AssetImportError("generated material-layer audit is missing or malformed")
    if report.get("schema") != MATERIAL_LAYER_AUDIT_SCHEMA:
        raise AssetImportError("generated material-layer audit schema is unsupported")

    alpha_counts = report.get("texture_alpha_classes")
    registration_counts = report.get("effective_registration_layers")
    suspicious = report.get("suspicious_cross_classifications")
    expected_alpha_keys = {alpha_class.value for alpha_class in TextureAlphaClass}
    expected_registration_keys = {
        "opaque_only",
        "cutout_only",
        "transparent_only",
        "mixed",
        "other_only",
        "empty",
    }
    if (
        not isinstance(alpha_counts, dict)
        or set(alpha_counts) != expected_alpha_keys
        or any(type(value) is not int or value < 0 for value in alpha_counts.values())
    ):
        raise AssetImportError("generated texture alpha-class audit is malformed")
    if (
        not isinstance(registration_counts, dict)
        or set(registration_counts) != expected_registration_keys
        or any(
            type(value) is not int or value < 0
            for value in registration_counts.values()
        )
    ):
        raise AssetImportError("generated registration-layer audit is malformed")
    if not isinstance(suspicious, list) or any(
        not isinstance(entry, dict)
        or set(entry) != {
            "identifier", "slot", "texture", "alpha_class", "render_layer"
        }
        or any(not isinstance(value, str) for value in entry.values())
        for entry in suspicious
    ):
        raise AssetImportError(
            "generated suspicious cross-classification audit is malformed"
        )

    if isinstance(counts, dict):
        if sum(alpha_counts.values()) != counts.get("textures"):
            raise AssetImportError(
                "generated texture alpha-class audit count is inconsistent"
            )
        if sum(registration_counts.values()) != counts.get("blocks"):
            raise AssetImportError(
                "generated registration-layer audit count is inconsistent"
            )


def source_version(
    archive: zipfile.ZipFile, entries: dict[str, zipfile.ZipInfo]
) -> str | None:
    for candidate in ("build_assets/version.txt", "base/version.txt"):
        info = entries.get(candidate)
        if info is None:
            continue
        try:
            version = archive.read(info).decode("utf-8").strip()
        except UnicodeDecodeError:
            continue
        if version and len(version) <= 128:
            return version
    return None


def synchronize(
    root: Path,
    explicit_jar: str | Path | None = None,
    *,
    force: bool = False,
    required_identifiers: tuple[str, ...] = REQUIRED_BLOCK_IDENTIFIERS,
) -> tuple[dict[str, object], bool]:
    jar, _ = resolve_jar(root, explicit_jar)
    with _source_jar_snapshot(root, jar) as (jar_snapshot, jar_digest):
        with _synchronization_lock(root, jar_digest):
            with _publication_guard(root):
                if not force:
                    provenance = _read_provenance(root)
                    if _import_matches(root, provenance, jar_digest):
                        assert provenance is not None
                        return provenance, False
                starting_publication = _publication_token(root)

            with _generated_tree_staging(root) as staging:
                diagnostics: list[str] = []
                model_audit = BlockModelImportAudit()
                collision_audit = BlockCollisionImportAudit()
                with zipfile.ZipFile(jar_snapshot) as archive:
                    entries = indexed_archive(archive)
                    if not any(
                        path.startswith("base/blocks/") and path.endswith(".json")
                        for path in entries
                    ):
                        raise AssetImportError(
                            "JAR has no Cosmic Reach base/blocks definitions"
                        )
                    extract_direct_assets(archive, entries, staging, diagnostics)
                    compile_blocks(
                        archive, entries, staging, diagnostics, model_audit,
                        collision_audit,
                    )
                    omit_blocks_with_unsupported_textures(
                        staging, diagnostics, model_audit, collision_audit
                    )
                    version = source_version(archive, entries)
                counts = validate_generated_tree(staging, required_identifiers)
                material_layer_audit = audit_generated_material_layers(staging)
                validate_material_layer_audit_report(
                    material_layer_audit, counts
                )
                collision_report = collision_audit.provenance()
                validate_block_collision_import_report(
                    collision_report,
                    audit_generated_collision_shapes(staging),
                )
                provenance = {
                    "schema": PROVENANCE_SCHEMA,
                    "jar_sha256": jar_digest,
                    "importer_schema": IMPORTER_SCHEMA,
                    "importer_sha256": importer_sha256(),
                    "source_prefix": SOURCE_PREFIX,
                    "output_tree_sha256": sha256_tree(staging),
                    "counts": counts,
                    "block_model_import": model_audit.provenance(),
                    "block_collision_import": collision_report,
                    "material_layer_audit": material_layer_audit,
                }
                if version is not None:
                    provenance["source_version"] = version
                if diagnostics:
                    provenance["source_omissions"] = diagnostics

                with _publication_guard(root, reap_staging=False):
                    if _publication_token(root) != starting_publication:
                        current = _read_provenance(root)
                        if _import_matches(root, current, jar_digest):
                            assert current is not None
                            return current, False
                        raise AssetImportError(
                            "generated assets changed while synchronization was "
                            "in progress; retry the synchronization"
                        )
                    _publish_generated_tree(root, staging, provenance)
                return provenance, True


def validate_existing_import(root: Path) -> dict[str, object]:
    with _publication_guard(root):
        return _validate_existing_import(root)


def _validate_existing_import(root: Path) -> dict[str, object]:
    provenance = _read_provenance(root)
    if provenance is None:
        raise AssetImportError("generated asset provenance is missing or malformed")
    if provenance.get("schema") != PROVENANCE_SCHEMA:
        raise AssetImportError("generated asset provenance schema is unsupported")
    if provenance.get("importer_schema") != IMPORTER_SCHEMA:
        raise AssetImportError("generated asset importer schema is unsupported")
    if provenance.get("importer_sha256") != importer_sha256():
        raise AssetImportError("generated assets require synchronization with this importer")
    validate_block_model_import_report(provenance.get("block_model_import"))
    validate_block_collision_import_report(
        provenance.get("block_collision_import")
    )
    assets = root / GENERATED_ASSETS_RELATIVE_PATH
    counts = validate_generated_tree(assets)
    validate_block_collision_import_report(
        provenance.get("block_collision_import"),
        audit_generated_collision_shapes(assets),
    )
    material_layer_audit = audit_generated_material_layers(assets)
    validate_material_layer_audit_report(material_layer_audit, counts)
    if provenance.get("material_layer_audit") != material_layer_audit:
        raise AssetImportError(
            "generated material-layer audit does not match the asset tree"
        )
    actual_hash = sha256_tree(assets)
    if provenance.get("output_tree_sha256") != actual_hash:
        raise AssetImportError("generated asset tree does not match provenance")
    if provenance.get("counts") != counts:
        raise AssetImportError("generated asset counts do not match provenance")
    return provenance


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


def _retry_replace(source: Path, destination: Path) -> None:
    try:
        os.replace(source, destination)
    except OSError:
        if not source.exists() and destination.exists():
            return
        os.replace(source, destination)


def _retry_remove_tree(path: Path) -> None:
    if not path.exists():
        return
    try:
        shutil.rmtree(path)
    except OSError:
        shutil.rmtree(path)


def _staging_lease_path(staging: Path) -> Path:
    return staging.with_name(staging.name + STAGING_LEASE_SUFFIX)


@contextmanager
def _generated_tree_staging(root: Path):
    workspace = root / ".rigel"
    workspace.mkdir(parents=True, exist_ok=True)
    lease: Path | None = None
    staging: Path | None = None
    lease_file = None
    with _publication_lock(root):
        descriptor, lease_name = tempfile.mkstemp(
            prefix=STAGING_PREFIX, suffix=STAGING_LEASE_SUFFIX, dir=workspace
        )
        lease = Path(lease_name)
        staging = lease.with_name(lease.name[: -len(STAGING_LEASE_SUFFIX)])
        try:
            lease_file = os.fdopen(descriptor, "a+b")
            fcntl.flock(lease_file.fileno(), fcntl.LOCK_EX)
            staging.mkdir()
        except BaseException:
            if lease_file is None:
                os.close(descriptor)
            else:
                lease_file.close()
            lease.unlink(missing_ok=True)
            raise
    assert lease is not None and staging is not None and lease_file is not None
    try:
        yield staging
    finally:
        try:
            with _publication_lock(root):
                if staging.exists():
                    shutil.rmtree(staging)
                lease.unlink(missing_ok=True)
        finally:
            fcntl.flock(lease_file.fileno(), fcntl.LOCK_UN)
            lease_file.close()


def _reap_abandoned_staging(root: Path) -> None:
    workspace = root / ".rigel"
    for staging in sorted(
        path for path in workspace.glob(f"{STAGING_PREFIX}*") if path.is_dir()
    ):
        lease = _staging_lease_path(staging)
        with lease.open("a+b") as lease_file:
            try:
                fcntl.flock(
                    lease_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB
                )
            except BlockingIOError:
                continue
            try:
                _retry_remove_tree(staging)
            finally:
                fcntl.flock(lease_file.fileno(), fcntl.LOCK_UN)
        lease.unlink(missing_ok=True)
    for lease in sorted(workspace.glob(f"{STAGING_PREFIX}*{STAGING_LEASE_SUFFIX}")):
        staging = lease.with_name(
            lease.name[: -len(STAGING_LEASE_SUFFIX)]
        )
        if staging.exists():
            continue
        try:
            with lease.open("r+b") as lease_file:
                try:
                    fcntl.flock(
                        lease_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB
                    )
                except BlockingIOError:
                    continue
                lease.unlink(missing_ok=True)
        except FileNotFoundError:
            continue


@contextmanager
def _publication_lock(root: Path):
    lock_path = root / PUBLICATION_LOCK_RELATIVE_PATH
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


@contextmanager
def _synchronization_lock(root: Path, jar_digest: str):
    workspace = root / ".rigel"
    workspace.mkdir(parents=True, exist_ok=True)
    lock_path = workspace / f"{SYNCHRONIZATION_LOCK_PREFIX}{jar_digest}.lock"
    with lock_path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


@contextmanager
def _publication_guard(root: Path, *, reap_staging: bool = True):
    with _publication_lock(root):
        _recover_interrupted_publication(root)
        if reap_staging:
            _reap_abandoned_staging(root)
        yield


def _filesystem_entry_token(path: Path) -> str | None:
    if path.is_dir():
        return "tree:" + sha256_tree(path)
    if path.is_file():
        return "file:" + sha256_file(path)
    return "other" if path.exists() else None


def _publication_token(root: Path) -> tuple[str | None, str | None]:
    return (
        _filesystem_entry_token(root / GENERATED_ASSETS_RELATIVE_PATH),
        _filesystem_entry_token(root / PROVENANCE_RELATIVE_PATH),
    )


def _tree_matches_provenance(
    assets: Path, provenance: dict[str, object] | None
) -> bool:
    expected_hash = provenance.get("output_tree_sha256") if provenance else None
    if not isinstance(expected_hash, str) or not assets.is_dir():
        return False
    try:
        return sha256_tree(assets) == expected_hash
    except (AssetImportError, OSError):
        return False


def _published_state_is_coherent(root: Path) -> bool:
    return _tree_matches_provenance(
        root / GENERATED_ASSETS_RELATIVE_PATH, _read_provenance(root)
    )


def _transaction_state(backup: Path) -> dict[str, object] | None:
    return _read_json_object(backup / "transaction.json")


def _retry_atomic_copy(source: Path, destination: Path) -> None:
    try:
        atomic_copy(source, destination)
    except OSError:
        atomic_copy(source, destination)


def _tree_matches_sha256(path: Path, expected: object) -> bool:
    if not isinstance(expected, str) or not path.is_dir():
        return False
    try:
        return sha256_tree(path) == expected
    except (AssetImportError, OSError):
        return False


def _file_matches_sha256(path: Path, expected: object) -> bool:
    if not isinstance(expected, str) or not path.is_file():
        return False
    try:
        return sha256_file(path) == expected
    except OSError:
        return False


def _restore_recorded_previous_publication(
    root: Path, backup: Path, transaction: dict[str, object]
) -> None:
    destination = root / GENERATED_ASSETS_RELATIVE_PATH
    provenance_path = root / PROVENANCE_RELATIVE_PATH
    previous_assets = backup / "assets"
    previous_provenance = backup / provenance_path.name
    discarded_assets = backup / "discarded-assets"
    had_previous_assets = transaction["had_previous_assets"]
    had_previous_provenance = transaction["had_previous_provenance"]
    assets_sha256 = transaction["previous_assets_sha256"]
    provenance_sha256 = transaction["previous_provenance_sha256"]

    if had_previous_assets:
        if _tree_matches_sha256(previous_assets, assets_sha256):
            if (
                destination.exists()
                and not _tree_matches_sha256(destination, assets_sha256)
            ):
                _retry_remove_tree(discarded_assets)
                _retry_replace(destination, discarded_assets)
            if not _tree_matches_sha256(destination, assets_sha256):
                _retry_replace(previous_assets, destination)
        elif not _tree_matches_sha256(destination, assets_sha256):
            raise AssetImportError(
                "cannot locate the generated asset tree retained for rollback"
            )
    elif destination.exists():
        _retry_remove_tree(discarded_assets)
        _retry_replace(destination, discarded_assets)

    if had_previous_provenance:
        if _file_matches_sha256(previous_provenance, provenance_sha256):
            _retry_atomic_copy(previous_provenance, provenance_path)
        elif not _file_matches_sha256(provenance_path, provenance_sha256):
            raise AssetImportError(
                "cannot locate the generated asset provenance retained for rollback"
            )
    else:
        provenance_path.unlink(missing_ok=True)

    assets_restored = (
        _tree_matches_sha256(destination, assets_sha256)
        if had_previous_assets
        else not destination.exists()
    )
    provenance_restored = (
        _file_matches_sha256(provenance_path, provenance_sha256)
        if had_previous_provenance
        else not provenance_path.exists()
    )
    if not assets_restored or not provenance_restored:
        raise AssetImportError(
            "generated asset publication recovery did not restore its prior state"
        )
    _retry_remove_tree(backup)


def _restore_previous_publication(root: Path, backup: Path) -> None:
    destination = root / GENERATED_ASSETS_RELATIVE_PATH
    provenance_path = root / PROVENANCE_RELATIVE_PATH
    previous_assets = backup / "assets"
    previous_provenance = backup / provenance_path.name
    discarded_assets = backup / "discarded-assets"
    transaction = _transaction_state(backup)
    recorded_state = (
        transaction is not None
        and isinstance(transaction.get("had_previous_assets"), bool)
        and isinstance(transaction.get("had_previous_provenance"), bool)
        and "previous_assets_sha256" in transaction
        and "previous_provenance_sha256" in transaction
    )
    if recorded_state:
        assert transaction is not None
        _restore_recorded_previous_publication(root, backup, transaction)
        return
    had_previous_assets = (
        transaction.get("had_previous_assets") if transaction else None
    )
    had_previous_provenance = (
        transaction.get("had_previous_provenance") if transaction else None
    )
    previous = _read_json_object(previous_provenance)
    declared_empty_state = (
        had_previous_assets is False and had_previous_provenance is False
    )
    legacy_empty_state = (
        transaction is None
        and not previous_assets.exists()
        and not previous_provenance.exists()
    )

    if _tree_matches_provenance(previous_assets, previous):
        if destination.exists():
            _retry_remove_tree(discarded_assets)
            _retry_replace(destination, discarded_assets)
        _retry_atomic_copy(previous_provenance, provenance_path)
        _retry_replace(previous_assets, destination)
    elif _tree_matches_provenance(destination, previous):
        _retry_atomic_copy(previous_provenance, provenance_path)
    elif declared_empty_state:
        if destination.exists():
            _retry_remove_tree(discarded_assets)
            _retry_replace(destination, discarded_assets)
        provenance_path.unlink(missing_ok=True)
    elif _published_state_is_coherent(root):
        # Publication failed before the previous generation was moved.
        pass
    elif legacy_empty_state:
        # This also recovers an interrupted first publication created by an
        # importer version that predates the transaction marker.
        if destination.exists():
            _retry_remove_tree(discarded_assets)
            _retry_replace(destination, discarded_assets)
        provenance_path.unlink(missing_ok=True)
    else:
        raise AssetImportError(
            "cannot restore the generated asset generation retained after "
            "an interrupted publication"
        )

    if not (
        _published_state_is_coherent(root)
        or (
            (declared_empty_state or legacy_empty_state)
            and not destination.exists()
            and not provenance_path.exists()
        )
    ):
        raise AssetImportError(
            "generated asset publication recovery did not produce a coherent state"
        )
    _retry_remove_tree(backup)


def _recover_interrupted_publication(root: Path) -> None:
    workspace = (root / GENERATED_ASSETS_RELATIVE_PATH).parent
    backups = sorted(workspace.glob(".assets-previous-*"))
    if not backups:
        return
    if _published_state_is_coherent(root):
        for backup in backups:
            _retry_remove_tree(backup)
        return
    if len(backups) != 1:
        raise AssetImportError(
            "cannot recover multiple interrupted generated asset publications"
        )
    _restore_previous_publication(root, backups[0])


def publish_generated_tree(root: Path, staging: Path, provenance: dict[str, object]) -> None:
    with _publication_guard(root, reap_staging=False):
        _publish_generated_tree(root, staging, provenance)


def _publish_generated_tree(
    root: Path, staging: Path, provenance: dict[str, object]
) -> None:
    destination = root / GENERATED_ASSETS_RELATIVE_PATH
    provenance_path = root / PROVENANCE_RELATIVE_PATH
    destination.parent.mkdir(parents=True, exist_ok=True)
    had_previous_assets = destination.exists()
    had_previous_provenance = provenance_path.is_file()
    previous_assets_sha256 = (
        sha256_tree(destination) if had_previous_assets else None
    )
    previous_provenance_sha256 = (
        sha256_file(provenance_path) if had_previous_provenance else None
    )
    backup = Path(
        tempfile.mkdtemp(prefix=".assets-previous-", dir=destination.parent)
    )
    previous_assets = backup / "assets"
    previous_provenance = backup / provenance_path.name
    transaction_path = backup / "transaction.json"
    try:
        atomic_write_json(
            transaction_path,
            {
                "had_previous_assets": had_previous_assets,
                "had_previous_provenance": had_previous_provenance,
                "previous_assets_sha256": previous_assets_sha256,
                "previous_provenance_sha256": previous_provenance_sha256,
            },
        )
    except BaseException:
        _retry_remove_tree(backup)
        _retry_remove_tree(staging)
        raise
    try:
        if had_previous_provenance:
            atomic_copy(provenance_path, previous_provenance)
        if had_previous_assets:
            os.replace(destination, previous_assets)
        os.replace(staging, destination)
        atomic_write_json(provenance_path, provenance)
    except BaseException:
        try:
            _restore_previous_publication(root, backup)
            _retry_remove_tree(staging)
        except BaseException as recovery_error:
            raise AssetImportError(
                "generated asset publication failed and rollback was incomplete"
            ) from recovery_error
        raise
    try:
        _retry_remove_tree(backup)
    except OSError:
        # The installed tree and provenance are already coherent. A cleanup
        # failure must not turn a committed publication into a reported failure.
        pass


def _matching_import_provenance(
    root: Path, jar_digest: str
) -> dict[str, object] | None:
    with _publication_guard(root):
        provenance = _read_provenance(root)
        if not _import_matches(root, provenance, jar_digest):
            return None
        return provenance


def current_import_matches(root: Path, jar_digest: str) -> bool:
    return _matching_import_provenance(root, jar_digest) is not None


def _is_sha256(value: str) -> bool:
    return len(value) == 64 and all(
        character in "0123456789abcdef" for character in value
    )


def _resolve_snapshot_output(root: Path, output: Path) -> Path:
    resolved_output = output.expanduser().resolve()
    assets = (root.expanduser().resolve() / GENERATED_ASSETS_RELATIVE_PATH).resolve()
    if (
        resolved_output == assets
        or assets in resolved_output.parents
        or resolved_output in assets.parents
    ):
        raise AssetImportError(
            "generated asset snapshot output must not overlap the generated asset tree"
        )
    return resolved_output


@contextmanager
def _snapshot_output_guard(output: Path, create: bool = False):
    if create:
        output.mkdir(parents=True, exist_ok=True)
    if not output.is_dir():
        raise AssetImportError(
            f"generated asset snapshot output does not exist: {output}"
        )
    lock_path = output / SNAPSHOT_LOCK_FILENAME
    with lock_path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _active_snapshot_hash(output: Path) -> str | None:
    marker = output / SNAPSHOT_ACTIVE_GENERATION_FILENAME
    if not marker.exists():
        return None
    try:
        document = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AssetImportError(
            f"generated asset snapshot handoff marker is invalid: {marker}"
        ) from error
    active_hash = document.get("tree_sha256") if isinstance(document, dict) else None
    if not isinstance(active_hash, str) or not _is_sha256(active_hash):
        raise AssetImportError(
            f"generated asset snapshot handoff marker is invalid: {marker}"
        )
    active = output / active_hash
    if (
        active.is_symlink()
        or not active.is_dir()
        or sha256_tree(active) != active_hash
    ):
        raise AssetImportError(
            f"active generated asset snapshot is not coherent: {active}"
        )
    return active_hash


def _snapshot_consumer_hash(output: Path, consumer: Path) -> str | None:
    consumer = consumer.expanduser().resolve()
    try:
        content = consumer.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    except (OSError, UnicodeDecodeError) as error:
        raise AssetImportError(
            f"generated asset snapshot consumer is unreadable: {consumer}"
        ) from error

    referenced_hashes = {
        path.name
        for path in output.iterdir()
        if _is_sha256(path.name) and f"{path}{os.sep}" in content
    }
    if len(referenced_hashes) > 1:
        raise AssetImportError(
            f"generated asset snapshot consumer references multiple generations: {consumer}"
        )
    if not referenced_hashes:
        return None

    referenced_hash = referenced_hashes.pop()
    referenced = output / referenced_hash
    if (
        referenced.is_symlink()
        or not referenced.is_dir()
        or sha256_tree(referenced) != referenced_hash
    ):
        raise AssetImportError(
            f"consumer-referenced generated asset snapshot is not coherent: {referenced}"
        )
    return referenced_hash


def _record_active_snapshot(output: Path, active_hash: str | None) -> None:
    marker = output / SNAPSHOT_ACTIVE_GENERATION_FILENAME
    if active_hash is None:
        marker.unlink(missing_ok=True)
        return
    atomic_write_json(marker, {"tree_sha256": active_hash})


def _retire_snapshot_candidates(output: Path, retained_hashes: set[str]) -> None:
    for path in sorted(output.iterdir()):
        name = path.name
        is_generation = _is_sha256(name)
        is_staging = (
            name.startswith(".")
            and name.endswith(SNAPSHOT_STAGING_SUFFIX)
            and _is_sha256(name[1 : -len(SNAPSHOT_STAGING_SUFFIX)])
        )
        if not is_staging and (not is_generation or name in retained_hashes):
            continue
        if path.is_symlink() or not path.is_dir():
            raise AssetImportError(f"unsafe generated asset snapshot entry: {path}")
        _retry_remove_tree(path)


def snapshot_generated_assets(
    root: Path,
    output: Path,
    expected_jar_digest: str,
    consumer: Path | None = None,
) -> Path:
    output = _resolve_snapshot_output(root, output)
    with _publication_guard(root):
        provenance = _read_provenance(root)
        assets = root / GENERATED_ASSETS_RELATIVE_PATH
        if not _tree_matches_provenance(assets, provenance):
            raise AssetImportError(
                "cannot snapshot generated assets without a coherent tree and provenance"
            )
        assert provenance is not None
        published_jar_digest = provenance.get("jar_sha256")
        if published_jar_digest != expected_jar_digest:
            raise AssetImportError(
                "cannot snapshot generated assets because the published source "
                "JAR changed after synchronization "
                f"(expected {expected_jar_digest}, found {published_jar_digest})"
            )
        tree_hash = provenance["output_tree_sha256"]
        assert isinstance(tree_hash, str)
        with _snapshot_output_guard(output, create=True):
            retained_hashes = {tree_hash}
            if consumer is not None:
                active_hash = _snapshot_consumer_hash(output, consumer)
                _record_active_snapshot(output, active_hash)
            else:
                active_hash = _active_snapshot_hash(output)
            if active_hash is not None:
                retained_hashes.add(active_hash)
            _retire_snapshot_candidates(output, retained_hashes)

            destination = output / tree_hash
            if destination.exists():
                if (
                    not destination.is_symlink()
                    and destination.is_dir()
                    and sha256_tree(destination) == tree_hash
                ):
                    return destination
                raise AssetImportError(
                    f"generated asset snapshot is not coherent: {destination}"
                )

            staging = output / f".{tree_hash}{SNAPSHOT_STAGING_SUFFIX}"
            _retry_remove_tree(staging)
            try:
                shutil.copytree(assets, staging)
                if sha256_tree(staging) != tree_hash:
                    raise AssetImportError(
                        "generated assets changed while creating a build snapshot"
                    )
                os.replace(staging, destination)
            finally:
                if staging.exists():
                    shutil.rmtree(staging)
            return destination


def retire_generated_asset_snapshots(output: Path, retained: Path) -> None:
    output = output.expanduser().resolve()
    retained = retained.expanduser().resolve()
    if retained.parent != output or not _is_sha256(retained.name):
        raise AssetImportError(
            "retained generated asset snapshot must be a content-addressed "
            "child of the snapshot output"
        )
    with _snapshot_output_guard(output):
        if (
            retained.is_symlink()
            or not retained.is_dir()
            or sha256_tree(retained) != retained.name
        ):
            raise AssetImportError(
                f"retained generated asset snapshot is not coherent: {retained}"
            )
        atomic_write_json(
            output / SNAPSHOT_ACTIVE_GENERATION_FILENAME,
            {"tree_sha256": retained.name},
        )
        _retire_snapshot_candidates(output, {retained.name})


def _import_matches(
    root: Path, provenance: dict[str, object] | None, jar_digest: str
) -> bool:
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
        validate_block_model_import_report(provenance.get("block_model_import"))
        validate_block_collision_import_report(
            provenance.get("block_collision_import"),
            audit_generated_collision_shapes(assets),
        )
        validate_material_layer_audit_report(
            provenance.get("material_layer_audit"), provenance.get("counts")
        )
    except AssetImportError:
        return False
    return _tree_matches_provenance(assets, provenance)


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


def _read_json_object(path: Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def _read_provenance(root: Path) -> dict[str, object] | None:
    return _read_json_object(root / PROVENANCE_RELATIVE_PATH)


def read_provenance(root: Path) -> dict[str, object] | None:
    with _publication_guard(root):
        return _read_provenance(root)


def print_material_layer_audit(report: object) -> None:
    validate_material_layer_audit_report(report)
    assert isinstance(report, dict)
    alpha_counts = report["texture_alpha_classes"]
    registration_counts = report["effective_registration_layers"]
    suspicious = report["suspicious_cross_classifications"]
    assert isinstance(alpha_counts, dict)
    assert isinstance(registration_counts, dict)
    assert isinstance(suspicious, list)
    print(
        "Material layers: "
        f"fully_opaque={alpha_counts['fully_opaque']}, "
        f"binary={alpha_counts['binary']}, "
        f"fractional={alpha_counts['fractional']}, "
        f"opaque_only={registration_counts['opaque_only']}, "
        f"cutout_only={registration_counts['cutout_only']}, "
        f"transparent_only={registration_counts['transparent_only']}, "
        f"mixed={registration_counts['mixed']}, "
        f"other_only={registration_counts['other_only']}, "
        f"empty={registration_counts['empty']}, "
        f"suspicious_cross_classifications={len(suspicious)}"
    )
    for entry in suspicious:
        assert isinstance(entry, dict)
        print(
            "Suspicious material layer: "
            f"{entry['identifier']} slot={entry['slot']} "
            f"texture={entry['texture']} alpha={entry['alpha_class']} "
            f"layer={entry['render_layer']}"
        )


def print_block_collision_import(report: object) -> None:
    validate_block_collision_import_report(report)
    assert isinstance(report, dict)
    print(
        "Block collision import: "
        + ", ".join(
            f"{name}={report[name]}"
            for name in (
                "empty",
                "full",
                "single_partial",
                "multi_box",
                "exact_derived",
                "conservative_fallback",
                "ambiguous",
            )
        )
    )


def status(root: Path, explicit: str | Path | None = None) -> int:
    try:
        jar, source = resolve_jar(root, explicit)
    except AssetImportError as error:
        print(str(error))
        return 1

    jar_digest = sha256_file(jar)
    provenance = _matching_import_provenance(root, jar_digest)
    synchronized = provenance is not None
    print(f"Cosmic Reach JAR: {jar} ({source})")
    print(f"JAR SHA-256: {jar_digest}")
    print(f"Generated assets: {'current' if synchronized else 'not synchronized'}")
    if provenance is not None:
        print_block_collision_import(
            provenance.get("block_collision_import")
        )
        print_material_layer_audit(provenance.get("material_layer_audit"))
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

    sync_parser = subparsers.add_parser(
        "sync", help="synchronize .rigel/assets from a developer-provided JAR"
    )
    sync_parser.add_argument("--jar", type=Path)
    sync_parser.add_argument("--force", action="store_true")

    snapshot_parser = subparsers.add_parser(
        "snapshot", help="copy a coherent generated tree for resource embedding"
    )
    snapshot_parser.add_argument("--output", type=Path, required=True)
    snapshot_parser.add_argument("--jar-sha256", required=True)
    snapshot_parser.add_argument("--consumer", type=Path)

    retire_parser = subparsers.add_parser(
        "retire-snapshots",
        help="retire generated snapshots after resource inputs are handed off",
    )
    retire_parser.add_argument("--output", type=Path, required=True)
    retire_parser.add_argument("--retain", type=Path, required=True)

    subparsers.add_parser(
        "validate", help="validate the generated tree and its provenance"
    )
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
        if args.command == "sync":
            provenance, changed = synchronize(root, args.jar, force=args.force)
            action = "Synchronized" if changed else "Already current"
            print(f"{action}: {root / GENERATED_ASSETS_RELATIVE_PATH}")
            print(f"JAR SHA-256: {provenance['jar_sha256']}")
            print(f"Output SHA-256: {provenance['output_tree_sha256']}")
            counts = provenance["counts"]
            if isinstance(counts, dict):
                print(
                    "Assets: "
                    + ", ".join(f"{name}={counts[name]}" for name in sorted(counts))
                )
            model_report = provenance.get("block_model_import")
            if isinstance(model_report, dict):
                print(
                    "Block model import: "
                    + ", ".join(
                        f"{name}={model_report[name]}"
                        for name in (
                            "candidate_states",
                            "newly_recovered_states",
                            "base_approximation_states",
                            "corrected_approximations",
                        )
                    )
                )
                model_omissions = model_report.get("omissions")
                if isinstance(model_omissions, dict):
                    for reason in sorted(model_omissions):
                        summary = model_omissions[reason]
                        if isinstance(summary, dict):
                            states = summary.get("block_states", [])
                            print(
                                f"Block model omission {reason}: "
                                f"states={len(states) if isinstance(states, list) else 0}, "
                                f"candidate_states={summary.get('candidate_states')}, "
                                "base_approximation_states="
                                f"{summary.get('base_approximation_states')}"
                            )
            print_block_collision_import(
                provenance.get("block_collision_import")
            )
            print_material_layer_audit(provenance.get("material_layer_audit"))
            omissions = provenance.get("source_omissions", [])
            if isinstance(omissions, list):
                for omission in omissions:
                    print(f"Warning: {omission}", file=sys.stderr)
            return 0
        if args.command == "snapshot":
            print(
                snapshot_generated_assets(
                    root, args.output, args.jar_sha256, args.consumer
                )
            )
            return 0
        if args.command == "retire-snapshots":
            retire_generated_asset_snapshots(args.output, args.retain)
            return 0
        if args.command == "validate":
            provenance = validate_existing_import(root)
            print(f"Generated assets are valid: {provenance['output_tree_sha256']}")
            print_block_collision_import(
                provenance.get("block_collision_import")
            )
            print_material_layer_audit(provenance.get("material_layer_audit"))
            return 0
    except AssetImportError as error:
        print(f"{TOOL_NAME}: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
