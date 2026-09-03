# Test Framework

This document describes the current test framework and how tests are built and
run in Rigel.

---

## 1. Overview

Rigel uses a minimal in-tree test harness located in `tests/`:

- Most tests are compiled into `Rigel_tests` and link against `RigelLib`.
- Profiler tests are compiled into `Rigel_profiler_tests` with an isolated
  profiler implementation so test instrumentation does not change `RigelLib`.
- Block asset failure coverage is compiled into
  `Rigel_block_asset_failure_tests` with a fixed embedded-resource fixture.
- A configured Cosmic Reach JAR adds
  `Rigel_generated_asset_integration_tests`, which exercises the generated
  registry, chunk mesh, texture upload, and render-submission path without
  storing generated content in Git.
- `Rigel_public_header_consumer` compiles and links representative public
  headers using only `RigelLib`'s declared interface.
- The harness is implemented in `tests/TestFramework.h` and
  `tests/TestFramework.cpp`.
- Tests are registered via a `TEST_CASE(Name)` macro and executed by
  `tests/main.cpp`.
- CTest integration runs each test executable as an aggregate suite. Test cases
  register with the harness when the executable starts.

There is no external testing library (Catch2, GoogleTest, etc.).

---

## 2. Build and Run

### 2.1 CMake Option

Tests are controlled by the CMake option:

- `RIGEL_BUILD_TESTS` (default `ON`)

To disable tests, append `-DRIGEL_BUILD_TESTS=OFF` to either configure command
below.

### 2.2 Build Targets

When enabled, CMake adds:

- `Rigel_tests` (test executable)
- `Rigel_profiler_tests` (profiler test executable)
- `Rigel_block_asset_failure_tests` (missing-resource test executable)
- `Rigel_public_header_consumer` (public dependency interface check)
- `Rigel_generated_asset_integration_tests` (only when a Cosmic Reach JAR is
  configured)
- `Rigel_application_launch_integration_tests` (only when a Cosmic Reach JAR is
  configured)

### 2.3 Running Tests

From the repository root, create independent Conan-backed Debug and Release
builds and run every registered CTest entry:

```bash
rigel_debug_build=../Rigel-build-debug
conan install . --output-folder="$rigel_debug_build" --build=missing \
  -s build_type=Debug
cmake -S . -B "$rigel_debug_build" \
  -DCMAKE_TOOLCHAIN_FILE="$rigel_debug_build/conan_toolchain.cmake" \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build "$rigel_debug_build" --parallel
ctest --test-dir "$rigel_debug_build" --output-on-failure --parallel

rigel_release_build=../Rigel-build-release
conan install . --output-folder="$rigel_release_build" --build=missing \
  -s build_type=Release
cmake -S . -B "$rigel_release_build" \
  -DCMAKE_TOOLCHAIN_FILE="$rigel_release_build/conan_toolchain.cmake" \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$rigel_release_build" --parallel
ctest --test-dir "$rigel_release_build" --output-on-failure --parallel
```

Those commands are source-only gates only when all three JAR selectors are
absent: the `RIGEL_COSMIC_REACH_JAR` CMake cache value, the environment
variable of the same name, and `.rigel/source/Cosmic-Reach.jar` in the source
tree. Use a clean checkout or archive with no `.rigel/` tree for an auditable
source-only run, and unset the environment variable. CMake then reports that
only source-owned runtime assets are available, and the generated-asset and
application-launch integration executables are not registered.

For the conditional Cosmic Reach 0.6.1 acceptance gate, configure a third
fresh Release build with the JAR selected explicitly, then build and run all
CTest entries:

```bash
rigel_real_release_build=../Rigel-build-real-release
rigel_cosmic_reach_jar=/absolute/path/to/Cosmic-Reach-0.6.1.jar
conan install . --output-folder="$rigel_real_release_build" --build=missing \
  -s build_type=Release
cmake -S . -B "$rigel_real_release_build" \
  -DCMAKE_TOOLCHAIN_FILE="$rigel_real_release_build/conan_toolchain.cmake" \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
  -DCMAKE_BUILD_TYPE=Release \
  -DRIGEL_COSMIC_REACH_JAR="$rigel_cosmic_reach_jar"
cmake --build "$rigel_real_release_build" --parallel
ctest --test-dir "$rigel_real_release_build" --output-on-failure --parallel
```

The exact 0.6.1 digest enables the strict generated-asset expectations. On
Linux that CTest entry requires EGL, clears X11 and Wayland display selection,
forces software OpenGL, and treats a skipped case as a failure. Generated
resources and the digest-addressed JAR copy remain in the external build and
ignored source asset trees rather than becoming repository fixtures.

You can also run the test executable directly:

```bash
../Rigel-build-debug/Rigel_tests --list
../Rigel-build-debug/Rigel_tests --filter StreamingPolicy
../Rigel-build-debug/Rigel_tests --verbose
../Rigel-build-debug/Rigel_profiler_tests --verbose
../Rigel-build-debug/Rigel_block_asset_failure_tests --verbose
```

Note: The test executables are not placed in `bin` by default; they live
in the build directory root unless you change CMake output paths.

---

## 3. CTest Integration

CTest registration is done in `CMakeLists.txt`. Aggregate suite entries run the
three harness executables plus the public-header consumer; the harness obtains
its test cases from each harness executable's runtime registry. A small
filtered-run entry verifies the runner's executed-test summary. Adding,
renaming, or reformatting a `TEST_CASE` does not change aggregate suite coverage;
if the case selected by the filtered-run check is renamed, that check fails
rather than silently omitting it.

On Unix builds where CMake provides EGL, the shared OpenGL fixture uses a
surfaceless pbuffer when no X11 or Wayland display is available. This executes
the same OpenGL upload and draw calls without a window or swap-control path.
On Linux, the exact generated-asset integration gate requires EGL, clears both
display variables, selects software OpenGL, and requires zero skipped cases.
Its focused real-source importer entry reads a digest-addressed JAR snapshot
from the build tree, keeping the closure and repeated-synchronization checks
coupled to the embedded generated-resource snapshot. The synthetic importer
suite always runs without a real-JAR environment selection.

The generated-asset integration renders labeled cube, slab, stair,
multi-cuboid, rotated, cropped-UV, transparent, out-of-cell, and alpha-cutout
specimens, plus a dense serpentine row turn and the culling diagnostic pad,
through the production `WorldView` and `FrameRenderer` path. It always reads
the resulting color and depth buffers. The alpha-cutout check confirms that
transparent crop texels expose the farther gallery floor, the water check
requires its authored blend strength, and the mixed table check distinguishes
opaque wood from fractional glass. The opaque slab check confirms that partial
opaque models are not blended. Shape-aware targeting checks exact owner, state,
distance, position, cardinal face and normal, and cuboid index for real slab,
stair, furniture, ladder, handrail, door, machine, two-sided crop, and piston
overhang registrations. Diagnostic targeting and labels are checked from the
same catalog metadata before capture. The main synthetic OpenGL suite also
exercises target-outline depth occlusion, F1 independence, state restoration,
resource teardown, and TAA history isolation through software EGL. For visual
review, set
`RIGEL_GALLERY_CAPTURE_DIRECTORY` to an absolute directory outside the source
tree. The test validates every representative before publishing the captures
as one complete set. It writes one vertically oriented PPM per view
and a `capture-manifest.txt` completion record:

```bash
rigel_gallery_captures=/absolute/path/to/gallery-captures
DISPLAY= WAYLAND_DISPLAY= LIBGL_ALWAYS_SOFTWARE=1 \
RIGEL_GALLERY_CAPTURE_DIRECTORY="$rigel_gallery_captures" \
  ../Rigel-build-debug/Rigel_generated_asset_integration_tests \
  --filter RenderGallerySpecimens
```

Captures are inspection evidence rather than checked-in fixtures. The test
does not compare pixel hashes, and rejects capture destinations inside the
source tree. Publication stages all files beside the destination and atomically
exchanges a complete replacement with the previous set. A failed handoff leaves
the prior directory continuously visible, and a successful exchange removes
the displaced set.

Run the main suite via CTest:

```bash
ctest --test-dir ../Rigel-build-debug -R '^Rigel_tests$'
```

Use the test executable's `--filter` option to run selected test cases.

---

## 4. Test Harness API

### 4.1 Test Registration

```cpp
TEST_CASE(MyTest) {
    // test body
}
```

Each `TEST_CASE` registers a static `TestCase` entry in a global registry.

### 4.2 Assertions

Available macros (all throw on failure):

- `CHECK(expr)`
- `CHECK_EQ(lhs, rhs)`
- `CHECK_NE(lhs, rhs)`
- `CHECK_NEAR(lhs, rhs, eps)`
- `CHECK_THROWS(stmt)`
- `CHECK_NO_THROW(stmt)`
- `SKIP_TEST("reason")`

Failures throw `TestFailure`; skips throw `TestSkip` and are counted separately.

### 4.3 CLI Flags

The test runner supports:

- `--list` : print all registered tests
- `--filter <substring>` : run tests whose names contain the substring
- `--verbose` : print `[PASS]` lines for successful tests

`--filter` also supports `--filter=substring`.

---

## 5. Adding New Tests

1) Create a new `.cpp` file under `tests/`.
2) Include the framework header:

```cpp
#include "TestFramework.h"
```

3) Add one or more `TEST_CASE` blocks.
4) Build and run `Rigel_tests`; the new test registers automatically at runtime.

Most tests link against `RigelLib`, so they can use core engine types directly.
Profiler tests instead compile their implementation in the focused profiler
test executable.

---

## 6. Known Limitations

- No fixtures or parameterized tests.
- CTest reports results at executable-suite granularity rather than per test
  case.
- The harness uses exceptions for control flow.
- There is no built-in per-test setup/teardown beyond static construction.

---

## Related Docs

- `docs/ConfigurationSystem.md`
- `docs/AssetSystem.md`
- `docs/PersistenceAPI.md`
