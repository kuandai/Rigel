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

The interactive runtime requires the separately supplied block texture tree at
`assets/textures/`. The tracked definitions under `assets/blocks/` refer to
files in that tree, and startup rejects missing or invalid block inputs instead
of creating an all-air world.

Populate `assets/textures/` before configuring CMake so the resource-embedding
step discovers the files. Reconfigure and rebuild after changing imported
assets. A source-only checkout without the imported textures can still build
and run `Rigel_tests`; representative block/material tests use in-memory pixel
data and do not require production textures.

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
