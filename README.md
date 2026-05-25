# orca-slice-engine

Pure C consumer of `slic3r.dll` — no C++ dependencies, no libslic3r headers.

**Architecture:**
```
orca-slice-engine (14 KB, C)
  │  #include "slic3r_c_api.h"    ← only header
  │  slic3r_create() / slic3r_slice() / slic3r_destroy()
  ▼
slic3r.dll (23 MB, self-contained)
  ├── SliceEngine full pipeline
  ├── libslic3r + CGAL + OCCT + Boost + OpenCV
  └── 7 pure C exports, zero C++ type leakage
```

## Build

Requires `slic3r.dll` and `slic3r.lib` from OrcaSlicer.

```bash
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH="<path/to/slic3r/dll/dir>" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build/Release/orca-slice-engine.exe` (~14 KB).

One-click from OrcaSlicer root:
```cmd
scripts\build_sdk_and_engine.bat
```

## Run

Requires `slic3r.dll` and its dependencies (OCCT, GMP/MPFR DLLs) on PATH.
These are bundled in the OrcaSlicer SDK directory.

```cmd
set PATH=%PATH%;<orca_root>\deps\build\OrcaSlicer_dep\usr\local\bin\occt
set PATH=%PATH%;<orca_root>\deps\build\OrcaSlicer_dep\usr\local\bin
set PATH=%PATH%;<orca_root>\build\src\libslic3r\Release
set ORCA_RESOURCES=<orca_root>\resources

orca-slice-engine.exe model.3mf -p 0
```

## Dependencies

- `slic3r.dll` (and its import library `slic3r.lib`)
- MSVC runtime (`VCRUNTIME140.dll`)
- That's it. No libslic3r. No CGAL. No OCCT. No Boost.

## C API

| Function | Purpose |
|----------|---------|
| `slic3r_create(resources_dir)` | Init presets, return opaque handle |
| `slic3r_slice(ctx, 3mf, output, params, stats)` | Full slice pipeline |
| `slic3r_destroy(ctx)` | Cleanup |
| `slic3r_get_error(ctx)` | Last error string |
| `slic3r_version()` | Version string |
| `slic3r_cancel(ctx)` | Async cancellation |
| `slic3r_is_cancelled(ctx)` | Check cancellation flag |

See `src/slic3r_c_api.h` for full documentation.
