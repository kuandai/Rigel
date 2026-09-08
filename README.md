# Rigel

Rigel is a voxel engine prototype. The current application provides:

* a normal local mode whose CPU simulation host owns two preloaded, pinned
  chunks, advances fixed ticks, validates semantic block edits, and publishes
  immutable changes to a separate graphical replica;
* coherent local checkpoints plus bounded state playback and command
  resimulation for that normal mode;
* a read-only block gallery that retains background chunk loading, generation,
  meshing, and distance-based streaming;
* OpenGL 4.1 rendering for voxel layers and entities, with player-controlled
  Shadows On/Off and internal temporal anti-aliasing and debug paths; and
* global player preferences, save-owned world identity and generator
  snapshots, strict graph-definition assets, and CR-format chunk/entity
  persistence.

The CMake project is currently versioned `0.0.0`, and debug builds identify
themselves as a Developer Preview. Linux with GCC and macOS with Apple Clang
are the tested native build environments. See
[`docs/README.md`](docs/README.md) for the implemented architecture and known
limitations.

Normal mode is not a general terrain-streaming game world. Its two chunks are
the complete exact authority domain: camera movement does not extend it, the
frontier is explicitly unavailable, and View Distance changes are rejected.
The free-fly camera is a trusted local observer whose finite pose is admitted
and recorded before an edit; it is not a network movement or player-controller
interface. See [`docs/SimulationAuthority.md`](docs/SimulationAuthority.md) for
the current limits, command outcomes, save policy, and replay envelope.

## Build Instructions

This project uses CMake for the build system and Conan (2.x) for dependency management. Native compilation is
tested on Linux with GCC and on macOS with Apple Clang for the host architecture
(Apple Silicon `arm64` or Intel `x86_64`). The build follows Conan's detected
profile and does not produce a universal binary.

### Prerequisites

Ensure you have the following installed:

* **C++ Compiler** GCC 12.2 or Apple Clang, supporting C++20
* **CMake** Version 3.20+
* **Conan 2.x** `pip install conan`
* **OpenGL** Version 4.1 core with GLSL 4.10 support. On macOS this is the
  last supported system OpenGL profile (Metal-backed).
* **macOS** Xcode Command Line Tools (`xcode-select --install`) and a Cocoa
  desktop session to run the app or OpenGL tests

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
format and rendering limits. Developers can inspect every successfully loaded
renderable registration in the read-only, ephemeral
[`block gallery`](docs/BlockGallery.md).

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

### Graphics-Disabled Semantic Validation

The simulation authority and its real semantic parser/world/generator path can
be built without discovering or linking the graphics dependency graph. The
Conan and CMake graphics options must agree:

```bash
rigel_cpu_build=../Rigel-build-cpu
conan install . --output-folder="$rigel_cpu_build" --build=missing \
  -s build_type=Release -o '&:with_graphics=False' \
  -c tools.cmake.cmaketoolchain:user_presets=""
cmake -S . -B "$rigel_cpu_build" \
  -DCMAKE_TOOLCHAIN_FILE="$rigel_cpu_build/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DRIGEL_BUILD_GRAPHICS=OFF
cmake --build "$rigel_cpu_build" --parallel 8 \
  --target RigelSemantic Rigel_cpu_semantic_tests
ctest --test-dir "$rigel_cpu_build" --output-on-failure \
  -R '^Rigel_cpu_semantic_(tests|linkage)$'
```

`RigelSemantic` is the CPU library, and `Rigel_cpu_semantic_tests` is the actual
CPU regression host used for edit, checkpoint, recovery, and resimulation
coverage. Rigel does not currently install a standalone dedicated-server
executable. The linkage CTest rejects GL, EGL, GLFW, and GLEW dependencies.

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
cmake --build build-release --parallel "$(getconf _NPROCESSORS_ONLN)" --target Rigel
```

To build and run the tests:

```bash
cmake --build build-release --parallel "$(getconf _NPROCESSORS_ONLN)" --target Rigel_tests
ctest --test-dir build-release --output-on-failure --parallel "$(getconf _NPROCESSORS_ONLN)"
```
