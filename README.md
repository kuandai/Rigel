# Rigel
High-performance, extensible voxel engine

## Build Instructions

This project uses CMake for the build system and Conan (2.x) for dependency management. Compilation has only been
tested on Linux.

### Prerequisites

Ensure you have the following installed:

* **C++ Compiler** GCC 12.2 supporting C++20. Clang may work, untested
* **CMake** Version 3.20+
* **Conan 2.x** `pip install conan`
* **OpenGL** Version 4.1 core with GLSL 4.10 support

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
