#!/usr/bin/env python3
"""Build an invented single-cuboid import fixture for runtime tests."""

from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import zipfile
import zlib


sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import rigel_assets


def _png() -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    pixels = b"".join(b"\0" + b"\xff\xff\xff\xff" * 16 for _ in range(16))
    return (
        rigel_assets.PNG_SIGNATURE
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
    )


def entries() -> dict[str, bytes]:
    encode = lambda value: json.dumps(value).encode("utf-8")
    return {
        "base/models/blocks/ledge.json": encode(
            {
                "textures": {
                    "surface": {
                        "fileName": "base:textures/blocks/ledge.png"
                    }
                },
                "cuboids": [
                    {
                        "localBounds": [-2, 0, 4, 18, 10, 12],
                        "inflate": 0.5,
                        "faces": {
                            "localPosX": {
                                "uv": [15, 2, 3, 14],
                                "uvRotation": 90,
                                "texture": "surface",
                                "ambientocclusion": False,
                                "cullFace": False,
                            },
                            "localNegY": {
                                "uv": [1, 15, 14, 1],
                                "texture": "surface",
                                "ambientocclusion": True,
                                "cullFace": True,
                            },
                        },
                    }
                ],
            }
        ),
        "base/block_state_generators/ledge.json": encode(
            {
                "generators": [
                    {
                        "stringId": "base:ledge_sideways",
                        "params": {"facing": "east"},
                        "modelName": "base:models/blocks/ledge.json",
                        "overrides": {"rotation": [0, 90, 0]},
                    }
                ]
            }
        ),
        "base/blocks/ledge.json": encode(
            {
                "stringId": "test:ledge",
                "defaultProperties": {
                    "modelName": "base:models/blocks/ledge.json"
                },
                "blockStates": {
                    "default": {
                        "stateGenerators": ["base:ledge_sideways"]
                    }
                },
            }
        ),
        "base/textures/blocks/ledge.png": _png(),
    }


def prepare(root: Path) -> None:
    jar = root / "single-cuboid.jar"
    root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(jar, "w") as archive:
        for name, data in entries().items():
            archive.writestr(name, data)
    rigel_assets.synchronize(
        root, jar, required_identifiers=("test:ledge", "test:ledge[facing=east]")
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: single_cuboid_fixture.py OUTPUT_ROOT")
    prepare(Path(sys.argv[1]).resolve())
