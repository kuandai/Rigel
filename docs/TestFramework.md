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

You can also run the test executable directly:

```bash
../Rigel-build-debug/Rigel_tests --list
../Rigel-build-debug/Rigel_tests --filter WorldConfigProvider
../Rigel-build-debug/Rigel_tests --verbose
../Rigel-build-debug/Rigel_profiler_tests --verbose
../Rigel-build-debug/Rigel_block_asset_failure_tests --verbose
```

Note: The test executables are not placed in `bin` by default; they live
in the build directory root unless you change CMake output paths.

---

## 3. CTest Integration

CTest registration is done in `CMakeLists.txt`. Aggregate suite entries run
the three test executables; the harness obtains its test cases from each
executable's runtime registry. A small filtered-run entry verifies the runner's
executed-test summary. Adding, renaming, or reformatting a `TEST_CASE` does not
change aggregate suite coverage; if the case selected by the filtered-run check
is renamed, that check fails rather than silently omitting it.

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
