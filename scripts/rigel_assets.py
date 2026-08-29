#!/usr/bin/env python3
"""Prepare local Cosmic Reach assets for Rigel without redistributing them."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
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
GENERATOR_OVERRIDE_KEYS = BLOCK_PROPERTY_KEYS | {"rotateTopBottom"}
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


def _rotate_vector(vector: tuple[int, int, int], rotation: object, context: str) -> tuple[int, int, int]:
    if rotation is None:
        return vector
    if not isinstance(rotation, list) or len(rotation) != 3:
        raise AssetImportError(f"{context}: rotation must contain three angles")
    try:
        angles = tuple(int(angle) % 360 for angle in rotation)
    except (TypeError, ValueError) as error:
        raise AssetImportError(f"{context}: rotation angles must be integers") from error
    if any(angle not in (0, 90, 180, 270) for angle in angles):
        raise AssetImportError(f"{context}: only right-angle rotations are supported")
    x, y, z = vector
    for _ in range(angles[0] // 90):
        y, z = -z, y
    for _ in range(angles[1] // 90):
        x, z = z, -x
    for _ in range(angles[2] // 90):
        x, y = -y, x
    return x, y, z


@dataclass(frozen=True)
class ResolvedModel:
    textures: dict[str, str]
    face_aliases: dict[str, str]
    transparent: bool
    culls_self: bool
    empty: bool
    full_cube: bool


class BlockModelResolver:
    def __init__(
        self,
        archive: zipfile.ZipFile,
        entries: dict[str, zipfile.ZipInfo],
    ) -> None:
        self.archive = archive
        self.entries = entries
        self.cache: dict[str, ResolvedModel] = {}
        self.active: set[str] = set()

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
                parent = self.resolve(str(document["parent"]))

            textures = dict(parent.textures)
            texture_entries = _expect_object(document.get("textures", {}), f"{source}.textures")
            for alias, raw_entry in texture_entries.items():
                entry = _expect_object(raw_entry, f"{source}.textures.{alias}")
                _reject_unknown_keys(entry, MODEL_TEXTURE_KEYS, f"{source}.textures.{alias}")
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

            face_aliases = dict(parent.face_aliases)
            empty = parent.empty
            full_cube = parent.full_cube
            if "cuboids" in document:
                cuboids = document["cuboids"]
                if not isinstance(cuboids, list):
                    raise AssetImportError(f"{source}.cuboids must be an array")
                empty = len(cuboids) == 0 and not document.get("planes")
                full_cube = False
                representative: dict[str, object] | None = None
                for index, raw_cuboid in enumerate(cuboids):
                    cuboid = _expect_object(raw_cuboid, f"{source}.cuboids[{index}]")
                    _reject_unknown_keys(cuboid, MODEL_CUBOID_KEYS, f"{source}.cuboids[{index}]")
                    bounds = cuboid.get("localBounds")
                    if not isinstance(bounds, list) or len(bounds) != 6 or not all(
                        isinstance(item, (int, float)) for item in bounds
                    ):
                        raise AssetImportError(f"{source}.cuboids[{index}].localBounds is invalid")
                    if representative is None or bounds == [0, 0, 0, 16, 16, 16]:
                        representative = cuboid
                        if bounds == [0, 0, 0, 16, 16, 16]:
                            break
                full_cube = (
                    len(cuboids) == 1
                    and representative is not None
                    and representative.get("localBounds") == [0, 0, 0, 16, 16, 16]
                )
                face_aliases = {}
                if representative is not None:
                    faces = _expect_object(
                        representative.get("faces", {}), f"{source}.cuboid.faces"
                    )
                    for face_name, raw_face in faces.items():
                        if face_name not in FACE_VECTORS:
                            raise AssetImportError(f"{source}: unsupported model face {face_name!r}")
                        face = _expect_object(raw_face, f"{source}.faces.{face_name}")
                        _reject_unknown_keys(face, MODEL_FACE_KEYS, f"{source}.faces.{face_name}")
                        alias = face.get("texture")
                        if not isinstance(alias, str):
                            raise AssetImportError(f"{source}.faces.{face_name}: texture must be a string")
                        face_aliases[face_name] = alias

            transparent = document.get("isTransparent", parent.transparent)
            culls_self = document.get("cullsSelf", parent.culls_self)
            if not isinstance(transparent, bool) or not isinstance(culls_self, bool):
                raise AssetImportError(f"{source}: transparency/culling values must be booleans")
            result = ResolvedModel(
                textures, face_aliases, transparent, culls_self, empty, full_cube
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
    if "modelName" in properties and not isinstance(properties["modelName"], str):
        raise AssetImportError(f"{context}.modelName must be a string")


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
    model: ResolvedModel, rotation: object, context: str
) -> dict[str, str]:
    result: dict[str, str] = {}
    for source_face, alias in model.face_aliases.items():
        vector = _rotate_vector(FACE_VECTORS[source_face], rotation, context)
        destination_face = VECTOR_FACES[vector]
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


def render_block_yaml(
    identifier: str,
    properties: dict[str, object],
    models: BlockModelResolver,
    context: str,
    texture_model_reference: str | None = None,
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
        )
    opaque = properties.get("isOpaque", not model.transparent)
    solid = not properties.get("walkThrough", False)
    if not isinstance(opaque, bool) or not isinstance(solid, bool):
        raise AssetImportError(f"{context}: opacity/solidity values are invalid")
    texture_faces = _resolved_face_textures(model, properties.get("rotation"), context)
    lines = [
        f"id: {_yaml_string(identifier)}",
        f"model: {'none' if model.empty else 'cube'}",
        f"opaque: {'true' if opaque else 'false'}",
        f"solid: {'true' if solid else 'false'}",
    ]
    if model.culls_self or properties.get("isFluid", False):
        lines.append("cull_same_type: true")
    layer = "opaque" if model.empty or (opaque and not model.transparent) else "transparent"
    lines.append(f"layer: {layer}")
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
    if texture_faces:
        lines.append("textures:")
        values = set(texture_faces.values())
        if len(values) == 1:
            lines.append(f"  all: {_yaml_string(next(iter(values)))}")
        elif (
            texture_faces["pos_x"] == texture_faces["neg_x"]
            == texture_faces["pos_z"] == texture_faces["neg_z"]
        ):
            lines.append(f"  top: {_yaml_string(texture_faces['pos_y'])}")
            lines.append(f"  bottom: {_yaml_string(texture_faces['neg_y'])}")
            lines.append(f"  sides: {_yaml_string(texture_faces['pos_x'])}")
        else:
            for face in ("pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z"):
                lines.append(f"  {face}: {_yaml_string(texture_faces[face])}")
    return ("\n".join(lines) + "\n").encode("utf-8")


def compile_blocks(
    archive: zipfile.ZipFile,
    entries: dict[str, zipfile.ZipInfo],
    staging: Path,
    diagnostics: list[str] | None = None,
) -> int:
    models = BlockModelResolver(archive, entries)
    generators = StateGeneratorResolver(archive, entries)
    generated: dict[str, tuple[bytes, str]] = {}
    omitted_non_cube_variants = 0
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
                    generated_properties.pop("rotateTopBottom", None)
                    if leaf.model_name is not None:
                        generated_properties["modelName"] = leaf.model_name
                    generated_model_name = generated_properties.get("modelName")
                    if (
                        not isinstance(generated_model_name, str)
                        or not models.resolve(generated_model_name).full_cube
                    ):
                        omitted_non_cube_variants += 1
                        continue
                    variants.append(
                        (generated_parameters, generated_properties, base_model_name)
                    )

            for variant_parameters, variant_properties, texture_model in variants:
                identifiers = [_block_identifier(base_identifier, variant_parameters)]
                if LEGACY_DEFAULT_STATE_ALIASES.get(base_identifier) == variant_parameters:
                    identifiers.append(base_identifier)
                for identifier in identifiers:
                    output = _generated_block_filename(identifier)
                    payload = render_block_yaml(
                        identifier,
                        variant_properties,
                        models,
                        source,
                        texture_model,
                    )
                    if identifier in generated:
                        previous_source = generated[identifier][1]
                        raise AssetImportError(
                            f"duplicate generated block identifier {identifier}: "
                            f"{previous_source} and {source}"
                        )
                    generated[identifier] = (payload, source)
                    write_output(staging, output, payload)
    if omitted_non_cube_variants and diagnostics is not None:
        diagnostics.append(
            f"omitted {omitted_non_cube_variants} generated non-cube block states: "
            "Rigel's current normalized block model supports cube/none geometry"
        )
    return len(generated)


def parse_generated_block(data: bytes, source: str) -> dict[str, object]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise AssetImportError(f"{source}: generated block YAML is not UTF-8") from error
    result: dict[str, object] = {}
    textures: dict[str, str] = {}
    in_textures = False
    for line_number, line in enumerate(lines, start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith("  "):
            if not in_textures or ":" not in line:
                raise AssetImportError(f"{source}:{line_number}: malformed generated YAML")
            key, raw_value = line.strip().split(":", 1)
            try:
                value = json.loads(raw_value.strip())
            except json.JSONDecodeError as error:
                raise AssetImportError(
                    f"{source}:{line_number}: texture path must be quoted"
                ) from error
            if key in textures or not isinstance(value, str):
                raise AssetImportError(f"{source}:{line_number}: invalid texture entry")
            textures[key] = value
            continue
        in_textures = False
        if ":" not in line:
            raise AssetImportError(f"{source}:{line_number}: malformed generated YAML")
        key, raw_value = line.split(":", 1)
        if key in result:
            raise AssetImportError(f"{source}:{line_number}: duplicate field {key}")
        value = raw_value.strip()
        if key == "textures":
            if value:
                raise AssetImportError(f"{source}:{line_number}: textures must be a map")
            in_textures = True
            result[key] = textures
        elif key == "id":
            try:
                decoded = json.loads(value)
            except json.JSONDecodeError as error:
                raise AssetImportError(f"{source}:{line_number}: id must be quoted") from error
            if not isinstance(decoded, str):
                raise AssetImportError(f"{source}:{line_number}: id must be a string")
            result[key] = decoded
        elif key in ("opaque", "solid", "cull_same_type"):
            if value not in ("true", "false"):
                raise AssetImportError(f"{source}:{line_number}: {key} must be boolean")
            result[key] = value == "true"
        elif key in ("emits_light", "light_attenuation"):
            try:
                result[key] = int(value)
            except ValueError as error:
                raise AssetImportError(f"{source}:{line_number}: {key} must be integer") from error
        else:
            result[key] = value
    allowed = {
        "id", "model", "opaque", "solid", "cull_same_type", "layer",
        "emits_light", "light_attenuation", "textures",
    }
    _reject_unknown_keys(result, allowed, source)
    required = {"id", "model", "opaque", "solid", "layer"}
    missing = sorted(required - set(result))
    if missing:
        raise AssetImportError(f"{source}: missing generated fields: {', '.join(missing)}")
    if result["model"] not in ("cube", "none"):
        raise AssetImportError(f"{source}: invalid model {result['model']!r}")
    if result["layer"] not in ("opaque", "cutout", "transparent", "emissive"):
        raise AssetImportError(f"{source}: invalid render layer {result['layer']!r}")
    for key in ("emits_light", "light_attenuation"):
        if key in result and not 0 <= int(result[key]) <= 15:
            raise AssetImportError(f"{source}: {key} is outside 0..15")
    allowed_texture_keys = {
        "all", "top", "bottom", "sides", "default", "pos_x", "neg_x",
        "pos_y", "neg_y", "pos_z", "neg_z",
    }
    if textures:
        _reject_unknown_keys(textures, allowed_texture_keys, f"{source}.textures")
        pattern_valid = (
            set(textures) == {"all"}
            or set(textures) == {"top", "bottom", "sides"}
            or set(textures)
            == {"pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z"}
        )
        if not pattern_valid:
            raise AssetImportError(f"{source}: generated texture mapping is incomplete")
    elif result["model"] != "none":
        raise AssetImportError(f"{source}: cube model has no textures")
    return result


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

    block_identifiers: dict[str, str] = {}
    referenced_textures: dict[str, str] = {}
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
        if isinstance(textures, dict):
            for texture in textures.values():
                referenced_textures[str(texture)] = logical

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
    for logical in model_paths:
        model = _expect_object(
            load_relaxed_json((root / logical).read_bytes(), logical), logical
        )
        textures = _expect_object(model.get("textures", {}), f"{logical}.textures")
        for reference in textures.values():
            if not isinstance(reference, str):
                raise AssetImportError(f"{logical}: entity texture reference must be a string")
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

    return {
        "blocks": len(block_identifiers),
        "textures": sum(
            path.startswith("textures/") and path.endswith(".png")
            for path in logical_paths
        ),
        "models": len(model_paths),
        "animations": len(animation_paths),
        "sounds": sum(
            path.startswith("sounds/") and path.endswith(".ogg")
            for path in logical_paths
        ),
    }


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
    jar_digest = sha256_file(jar)
    if not force and current_import_matches(root, jar_digest):
        provenance = read_provenance(root)
        assert provenance is not None
        return provenance, False

    workspace = root / ".rigel"
    workspace.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".assets-staging-", dir=workspace))
    diagnostics: list[str] = []
    try:
        with zipfile.ZipFile(jar) as archive:
            entries = indexed_archive(archive)
            if not any(
                path.startswith("base/blocks/") and path.endswith(".json")
                for path in entries
            ):
                raise AssetImportError("JAR has no Cosmic Reach base/blocks definitions")
            extract_direct_assets(archive, entries, staging, diagnostics)
            compile_blocks(archive, entries, staging, diagnostics)
            version = source_version(archive, entries)
        counts = validate_generated_tree(staging, required_identifiers)
        provenance: dict[str, object] = {
            "schema": PROVENANCE_SCHEMA,
            "jar_sha256": jar_digest,
            "importer_schema": IMPORTER_SCHEMA,
            "importer_sha256": importer_sha256(),
            "source_prefix": SOURCE_PREFIX,
            "output_tree_sha256": sha256_tree(staging),
            "counts": counts,
        }
        if version is not None:
            provenance["source_version"] = version
        if diagnostics:
            provenance["source_omissions"] = diagnostics
        publish_generated_tree(root, staging, provenance)
        return provenance, True
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def validate_existing_import(root: Path) -> dict[str, object]:
    provenance = read_provenance(root)
    if provenance is None:
        raise AssetImportError("generated asset provenance is missing or malformed")
    if provenance.get("schema") != PROVENANCE_SCHEMA:
        raise AssetImportError("generated asset provenance schema is unsupported")
    assets = root / GENERATED_ASSETS_RELATIVE_PATH
    counts = validate_generated_tree(assets)
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
    synchronized = current_import_matches(root, jar_digest)
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

    sync_parser = subparsers.add_parser(
        "sync", help="synchronize .rigel/assets from a developer-provided JAR"
    )
    sync_parser.add_argument("--jar", type=Path)
    sync_parser.add_argument("--force", action="store_true")

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
            omissions = provenance.get("source_omissions", [])
            if isinstance(omissions, list):
                for omission in omissions:
                    print(f"Warning: {omission}", file=sys.stderr)
            return 0
        if args.command == "validate":
            provenance = validate_existing_import(root)
            print(f"Generated assets are valid: {provenance['output_tree_sha256']}")
            return 0
    except AssetImportError as error:
        print(f"{TOOL_NAME}: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
