#!/usr/bin/env python3
"""
Convert Cosmic Reach animation JSON into Rigel's animation format.

Usage:
  python3 scripts/convert_cr_animations.py <input> [--output OUT] [--suffix .rigel.json] [--overwrite]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, Tuple


def warn(message: str) -> None:
    print(f"[convert_cr_animations] {message}", file=sys.stderr)


def parse_float(value: Any, context: str) -> float | None:
    try:
        return float(value)
    except (TypeError, ValueError):
        warn(f"Skipping invalid float '{value}' in {context}")
        return None


def normalize_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        return value.strip().lower() in {"true", "1", "yes", "y"}
    return False


def convert_track(track: Any, context: str) -> list[dict[str, Any]]:
    if isinstance(track, list):
        return track
    if not isinstance(track, dict):
        warn(f"Skipping invalid track '{context}'")
        return []

    entries: list[Tuple[float, dict[str, Any]]] = []
    for time_key, value in track.items():
        time = parse_float(time_key, f"{context} time")
        if time is None:
            continue
        if not isinstance(value, (list, tuple)) or len(value) < 3:
            warn(f"Skipping invalid value in {context} at time {time_key}")
            continue
        entries.append((time, {"time": time, "value": [value[0], value[1], value[2]]}))

    entries.sort(key=lambda item: item[0])
    return [entry for _, entry in entries]


def convert_animation(data: Dict[str, Any], source: Path) -> Dict[str, Any]:
    out: Dict[str, Any] = {"animations": {}}
    animations = data.get("animations", {})
    if not isinstance(animations, dict):
        warn(f"No animations map in {source}")
        return out

    for name, anim in animations.items():
        if not isinstance(anim, dict):
            continue
        duration = anim.get("duration", anim.get("animation_length", 0.0))
        duration_value = parse_float(duration, f"{source}:{name} duration")
        if duration_value is None:
            duration_value = 0.0

        converted: Dict[str, Any] = {
            "duration": duration_value,
            "loop": normalize_bool(anim.get("loop", False)),
            "bones": {},
        }

        bones = anim.get("bones", {})
        if isinstance(bones, dict):
            for bone_name, bone in bones.items():
                if not isinstance(bone, dict):
                    continue
                bone_out: Dict[str, Any] = {}
                for channel in ("position", "rotation", "scale"):
                    if channel in bone:
                        bone_out[channel] = convert_track(
                            bone[channel], f"{source}:{name}:{bone_name}:{channel}"
                        )
                if bone_out:
                    converted["bones"][bone_name] = bone_out

        out["animations"][name] = converted

    return out


def default_output_name(path: Path, suffix: str) -> Path:
    name = path.name
    if name.endswith(".animation.json"):
        base = name[: -len(".animation.json")]
        return path.with_name(f"{base}{suffix}")
    return path.with_suffix(path.suffix + suffix)


def iter_input_files(path: Path) -> Iterable[Path]:
    if path.is_dir():
        yield from path.rglob("*.animation.json")
    else:
        yield path


def convert_file(in_path: Path, out_path: Path, overwrite: bool) -> bool:
    if out_path.exists() and not overwrite:
        warn(f"Skipping {in_path}; output exists: {out_path}")
        return False

    try:
        data = json.loads(in_path.read_text())
    except json.JSONDecodeError as exc:
        warn(f"Failed to parse {in_path}: {exc}")
        return False

    converted = convert_animation(data, in_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(converted, indent=2, ensure_ascii=True) + "\n")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert CR animation JSON to Rigel format.")
    parser.add_argument("input", help="Input file or directory of .animation.json files")
    parser.add_argument("--output", help="Output file or directory")
    parser.add_argument("--suffix", default=".rigel.json", help="Suffix for converted files")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing output")
    args = parser.parse_args()

    input_path = Path(args.input)
    output = Path(args.output) if args.output else None

    converted_any = False
    for in_path in iter_input_files(input_path):
        if output and output.suffix and input_path.is_file():
            out_path = output
        elif output:
            rel = in_path.relative_to(input_path)
            out_path = output / rel.parent / default_output_name(in_path, args.suffix).name
        else:
            out_path = default_output_name(in_path, args.suffix)

        if convert_file(in_path, out_path, args.overwrite):
            converted_any = True

    return 0 if converted_any else 1


if __name__ == "__main__":
    raise SystemExit(main())
