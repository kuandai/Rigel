# Rigel

Rigel is a voxel engine prototype. The current application opens a single local
world and provides:

* deterministic, seed-based terrain generation with climate and biome
  selection, caves, surface materials, and simple structures;
* background chunk loading, generation, meshing, and distance-based streaming;
* OpenGL 4.1 rendering for voxel layers and entities, with player-controlled
  Shadows On/Off and internal temporal anti-aliasing and debug paths; and
* global player preferences, save-owned world identity and generator
  snapshots, strict graph-definition assets, and CR-format chunk/entity
  persistence.

The CMake project is currently versioned `0.0.0`, and debug builds identify
themselves as a Developer Preview. Linux with GCC is the tested build
environment; other platforms and Clang are not currently verified. See
[`docs/README.md`](docs/README.md) for the implemented architecture and known
limitations.

## Build Instructions

This project uses CMake for the build system and Conan (2.x) for dependency management. Compilation has only been
tested on Linux.

### Prerequisites

Ensure you have the following installed:

* **C++ Compiler** GCC 12.2 supporting C++20. Clang may work, untested
* **CMake** Version 3.20+
* **Conan 2.x** `pip install conan`
* **OpenGL** Version 4.1 core with GLSL 4.10 support

### Interactive Runtime Assets

Git contains Rigel-owned assets only. The interactive runtime also needs assets
generated from a developer-provided Cosmic Reach JAR; Rigel does not download
or redistribute that JAR.

After obtaining the JAR legitimately, stage and synchronize it with:

```bash
python3 scripts/rigel_assets.py stage ~/Downloads/Cosmic-Reach.jar
python3 scripts/rigel_assets.py sync
```

Both the staged source and deterministic output live under the ignored
`.rigel/` directory. `status` reports whether the output matches the current
JAR and importer, while `validate` checks the generated tree and provenance:

```bash
python3 scripts/rigel_assets.py status
python3 scripts/rigel_assets.py validate
```

The importer resolves compatible source block models into Rigel-owned
normalized cuboid assets under `.rigel/assets/models/blocks/`. Runtime code
consumes those generated assets, not Cosmic Reach JSON. The supported boundary
is intentionally limited to the measured axis-aligned cuboid and right-angle
block-state cases; see
[`docs/AssetSystem.md`](docs/AssetSystem.md#normalized-block-models) for the
format and rendering limits.

CMake synchronizes before enumerating embedded resources whenever it finds a
JAR. An automated environment can provide an absolute path without staging:

```bash
cmake -S . -B build-release \
  -DRIGEL_COSMIC_REACH_JAR=/absolute/path/Cosmic-Reach.jar \
  ...
```

The `RIGEL_COSMIC_REACH_JAR` environment variable is also supported. Resolution
priority is the CMake cache path, the environment variable, then
`.rigel/source/Cosmic-Reach.jar`.

A source-only checkout with no JAR still configures, builds, and runs the unit
tests. Attempting interactive world startup without generated CR assets fails
with preparation instructions rather than silently creating an incomplete
world.

### One-Time Setup

If this is your first time using Conan on this machine, you must create a default profile to detect your compiler:

```bash
conan profile detect --force

# Allow conan to invoke package manager
cat <<EOF>>~/.conan2/profiles/default
[conf]
tools.system.package_manager:mode=install
tools.system.package_manager:sudo=True
EOF
```

Then install dependencies, configure CMake with the Conan toolchain, and compile:

```bash
conan install . --output-folder=build-release --build=missing
cmake -S . -B build-release \
  -DCMAKE_TOOLCHAIN_FILE=build-release/conan_toolchain.cmake \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel $(nproc) --target Rigel
```

To build and run the tests:

```bash
cmake --build build-release --parallel $(nproc) --target Rigel_tests
ctest --test-dir build-release --output-on-failure --parallel $(nproc)
```
