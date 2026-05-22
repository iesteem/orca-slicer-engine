# orca-slice-engine

Standalone cloud slicing engine — headless CLI slicer extracted from OrcaSlicer.

## Prerequisites

- CMake >= 3.13
- C++17 compiler (MSVC 2022 / GCC 11+ / Clang 14+)
- Precompiled libslic3r SDK (generated from OrcaSlicer)

## Quick Start

### 1. Generate libslic3r SDK (in OrcaSlicer repo)

```bash
cd OrcaSlicer
cmake --build build --target libslic3r libslic3r_cgal --config Release
cmake --install build --prefix ./build/sdk --config Release

# Bundle transitive dependencies
cp -rn deps/build/OrcaSlicer_dep/usr/local/include/* build/sdk/include/
cp -rn deps/build/OrcaSlicer_dep/usr/local/lib/*.lib build/sdk/lib/
cp -rn deps/build/OrcaSlicer_dep/usr/local/lib/cmake/* build/sdk/lib/cmake/

# Copy deps_src built libs (admesh, clipper, qhull, etc.)
find build/deps_src build/lib -name "*.lib" -path "*/Release/*" \
    -exec cp {} build/sdk/lib/ \;
```

### 2. Build engine

```bash
cd orca-slice-engine
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH="/path/to/OrcaSlicer/build/sdk" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j $(nproc)
```

### 3. Run

```bash
./orca-slice-engine -i model.3mf -o output.gcode.3mf -p 0
```

## Relationship with OrcaSlicer

This repo consumes libslic3r as a precompiled SDK via `find_package(libslic3r)`.
When libslic3r changes in the main OrcaSlicer repo, regenerate the SDK and rebuild.

**ABI requirement:** The SDK must be built with the same compiler version and
runtime library configuration as this repo. Recommended: use CI with identical
Docker images / MSVC toolchain versions for both repos.
