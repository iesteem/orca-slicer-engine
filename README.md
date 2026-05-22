# orca-slice-engine

Standalone cloud slicing engine — headless CLI slicer extracted from OrcaSlicer.

## Prerequisites

- CMake >= 3.13
- Visual Studio 2022 (MSVC 17.x)
- OrcaSlicer source repo (for SDK generation)

## One-Click Build

From the OrcaSlicer repo root:

```cmd
scripts\build_sdk_and_engine.bat          # Release (default)
scripts\build_sdk_and_engine.bat Debug    # Debug
```

This script handles the full pipeline:
1. Configure OrcaSlicer (headless)
2. Build `libslic3r` + `libslic3r_cgal`
3. Install SDK + bundle all transitive dependencies (Boost, CGAL, OpenCV, OCCT, etc.)
4. Configure and build `orca-slice-engine`

Output:
- SDK → `OrcaSlicer/build/sdk/`
- Engine → `orca-slice-engine/build/Release/orca-slice-engine.exe`

## Manual Build

### 1. Generate libslic3r SDK (in OrcaSlicer repo)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSLIC3R_GUI=OFF
cmake --build build --target libslic3r libslic3r_cgal --config Release
cmake --install build --prefix ./build/sdk --config Release

# Then run the bundling steps from scripts/build_sdk_and_engine.bat
```

### 2. Build engine

```bash
cd orca-slice-engine
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH="../../OrcaSlicer/build/sdk" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### 3. Run

The engine requires OCCT DLLs (and GMP/MPFR) at runtime.
These are bundled in `OrcaSlicer/build/sdk/lib/`.

```cmd
set PATH=%PATH%;C:\path\to\OrcaSlicer\build\sdk\lib
orca-slice-engine.exe model.3mf -p 0 -r resources/
```

## Relationship with OrcaSlicer

This repo consumes libslic3r as a precompiled SDK via `find_package(libslic3r)`.
When libslic3r changes in the main OrcaSlicer repo, regenerate the SDK and rebuild.

**ABI requirement:** The SDK must be built with the same compiler version and
runtime library configuration as this repo.
