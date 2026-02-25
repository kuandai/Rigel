# Test Framework

This document describes the current test framework and how tests are built and
run in Rigel.

---

## 1. Overview

Rigel uses a minimal in-tree test harness located in `tests/`:

- Tests are compiled into a single executable: `Rigel_tests`.
- The harness is implemented in `tests/TestFramework.h` and
  `tests/TestFramework.cpp`.
- Tests are registered via a `TEST_CASE(Name)` macro and executed by
  `tests/main.cpp`.
- CTest integration registers one CTest entry per `TEST_CASE` at configure
  time.

There is no external testing library (Catch2, GoogleTest, etc.).

---

## 2. Build and Run

### 2.1 CMake Option

Tests are controlled by the CMake option:

- `RIGEL_BUILD_TESTS` (default `ON`)

Disable with:

```bash
cmake -S . -B build -DRIGEL_BUILD_TESTS=OFF
```

### 2.2 Build Targets

When enabled, CMake adds:

- `Rigel_tests` (test executable)
- `coverage` (only when `RIGEL_ENABLE_COVERAGE=ON`)

### 2.3 Coverage Target

Coverage is controlled by:

- `RIGEL_ENABLE_COVERAGE` (default `OFF`)

Enable and run:

```bash
cmake -S . -B build -DRIGEL_BUILD_TESTS=ON -DRIGEL_ENABLE_COVERAGE=ON
cmake --build build --target coverage
```

Requirements:

- GNU toolchain (`--coverage`/gcov-compatible output)
- `lcov` and `genhtml` available in `PATH`

Output:

- HTML report at `build/coverage_html/index.html`

### 2.4 Running Tests

Typical commands:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

You can also run the test executable directly:

```bash
./build/Rigel_tests --list
./build/Rigel_tests --filter WorldConfigProvider
./build/Rigel_tests --verbose
```

Note: `Rigel_tests` is not placed in `build/bin` by default; it lives in the
build directory root unless you change CMake output paths.

---

## 3. CTest Integration

CTest registration is done in `CMakeLists.txt`:

- CMake scans `tests/*.cpp` at configure time.
- It extracts `TEST_CASE` names via regex:

```
TEST_CASE\(([A-Za-z0-9_]+)\)
```

- Each test becomes a CTest entry: `Rigel_<TestName>`.

Important implications:

- Test names must be alphanumeric/underscore only (no spaces or quotes).
- If you add or rename tests, re-run CMake to refresh the test list.

Run a single test via CTest:

```bash
ctest --test-dir build -R Rigel_WorldConfigProvider_FileSource
```

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
4) Re-run CMake so CTest discovers the new test names.

Tests link against `RigelLib`, so they can use core engine types directly.

---

## 6. Known Limitations

- No fixtures, parameterized tests, or test discovery at runtime.
- Test discovery is regex-based and runs only at CMake configure time.
- The harness uses exceptions for control flow.
- There is no built-in per-test setup/teardown beyond static construction.

---

## Related Docs

- `docs/ConfigurationSystem.md`
- `docs/AssetSystem.md`
- `docs/PersistenceAPI.md`
