# orca-slice-engine

Pure C consumer of `slic3r.dll` — slice a 3MF file to G-code with no C++
dependencies, no libslic3r headers, and a ~14 KB executable.

**Current version:** `v02.01.04` · See [CHANGELOG.md](CHANGELOG.md) for full history.

## Features

- **Pure-C DLL consumer.** Compile-time dependency is a single import library
  (`slic3r.lib`) and one header (`slic3r_c_api.h`). No libslic3r / CGAL / OCCT
  / Boost headers leak into the consumer.
- **Snapmaker-only build.** Official Snapmaker U1 printer / filament / process
  presets are enforced by default, with a `--skip-preset-substitution` escape
  hatch.
- **Per-plate slicing.** Slice a single plate, all plates, or anything in
  between via `-p / --plate`.
- **Statistics output.** Slice statistics (filament usage, time, plate info)
  are emitted as JSON via `-j / --json`.
- **Thumbnail passthrough.** Plate thumbnails are read from disk and embedded
  in the G-code as `; thumbnail` headers.
- **Cancellation & timeout.** External watchdog file (`--cancel-file`) and a
  built-in deadline (`--timeout`) for batch / cloud use.

## Quick Start

```bash
git clone https://github.com/iesteem/orca-slice-engine.git
cd orca-slice-engine

# Point CMAKE_PREFIX_PATH at the extracted slic3r SDK
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH="<path/to/slic3r_sdk>" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Run it:

```powershell
# PowerShell
$env:PATH += ";<path/to/slic3r_sdk>/bin"
$env:ORCA_RESOURCES = "<path/to/slic3r_sdk>/resources"
.\build\Release\orca-slice-engine.exe model.3mf -v
```

Output: `model.gcode` and `model.gcode.3mf` next to the input.

## CLI Reference

```
orca-slice-engine.exe <input.3mf> [options]

  -o, --output <path>      Output path (without extension, default: derived from input)
  -p, --plate <id>         Plate ID (0 = all plates, default: 0)
  -f, --format <fmt>       Output format: gcode | gcode.3mf (default: gcode.3mf)
  -r, --resources <dir>    Resources directory (or set ORCA_RESOURCES env var)
  --log [file]             Log file path (without .log extension; default: derived from output)
  -j, --json [file]        JSON statistics output path (without .json extension; default: derived from output)
  -t, --timeout <sec>      Slicing timeout in seconds (0 = no limit)
  --max-size <mb>          Max input file size in MB (0 = no limit)
  --cancel-file <file>     Watchdog file for external cancellation
  --skip-preset-substitution
                           Skip official preset enforcement
  -v, --verbose            Verbose output
  -h, --help               Show this help
```

## Architecture

```
orca-slice-engine (~14 KB, pure C)
  │  #include "slic3r_c_api.h"    ← only header
  │  slic3r_create() / slic3r_slice() / slic3r_destroy()
  ▼
slic3r.dll (self-contained)
  ├── SliceEngine full pipeline
  ├── libslic3r + CGAL + OCCT + Boost + OpenCV
  └── 7 pure C exports, zero C++ type leakage
```

The consumer / DLL boundary is layered to minimise churn when OrcaSlicer
upstream changes:

```
┌────────────────────────────────────────────┐
│  main.c  (pure C consumer)                 │  ← this repo
│  depends on: slic3r_c_api.h (7 C functions)│
├────────────────────────────────────────────┤
│  slic3r_c_api.h   ← ABI boundary           │  ← if stable, no consumer change
├────────────────────────────────────────────┤
│  slic3r.dll                                │
│  ├── SliceEngine.cpp  ← uses libslic3r     │  ← this repo (DLL source)
│  └── JsonReport.cpp / GeometryCheck.cpp    │
├────────────────────────────────────────────┤
│  libslic3r + CGAL + OCCT + Boost           │  ← upstream OrcaSlicer
└────────────────────────────────────────────┘
```

| Upstream change | Impact | Action |
|----------------|--------|--------|
| libslic3r internal implementation | None | Swap DLL, done |
| libslic3r header API changes | `SliceEngine.cpp` may break | Update engine source, rebuild DLL |
| `slic3r_c_api.h` signature changes | Consumer needs recompile | Update header, rebuild exe |
| `resources/` format changes | Runtime may fail | Replace `resources/` directory |

## Prerequisites

- CMake ≥ 3.13
- MSVC 2022 (or any C11 compiler)
- **slic3r SDK** — a self-contained bundle of `slic3r.dll`, `slic3r.lib`, and
  all runtime dependencies. You do **not** need a full OrcaSlicer source tree.

## Getting the slic3r SDK

### Option A: Download pre-built (easiest)

Download `slic3r_sdk_x64_Release.zip` from GitHub Releases (or your internal
file server) and extract it.

### Option B: Package from an existing OrcaSlicer build

```bash
# From the orca-slice-engine repo root:
./scripts/package_sdk.sh /path/to/OrcaSlicer
```

This traces all DLL dependencies, copies the `resources/` preset directory,
and produces `slic3r_sdk_x64_Release.zip` (~119 MB).

**Prerequisites for the script:** bash (MSYS2 / git-bash) with `objdump`, and
PowerShell 7+ for the zip step.

Contents of the SDK:

```
slic3r_sdk/
├── lib/
│   └── slic3r.lib           ← import library (compile-time)
├── bin/
│   ├── slic3r.dll           ← core slicing DLL
│   ├── TK*.dll              ← OCCT runtime (38 files)
│   ├── libgmp-10.dll        ← GMP/MPFR math
│   ├── libmpfr-4.dll
│   ├── msvcp140.dll         ← VC++ runtime
│   ├── vcruntime140.dll
│   └── vcruntime140_1.dll
└── resources/               ← printer/filament/process presets
```

## Build Modes

The `CMakeLists.txt` supports two modes:

### Consumer mode (default)

Builds only `orca-slice-engine.exe` against a prebuilt `slic3r.dll`:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdk_dir>
cmake --build build --config Release
```

### DLL mode (opt-in)

Builds `slic3r.dll` from engine source against a full libslic3r SDK:

```bash
cmake -S . -B build -DBUILD_SLIC3R_DLL=ON -DCMAKE_PREFIX_PATH=<OrcaSlicer SDK dir>
```

This requires the complete OrcaSlicer dependency chain (libslic3r, CGAL, OCCT,
Boost, TBB, OpenSSL). Use this when iterating on the DLL internals.

## Run

All runtime DLLs (OCCT, GMP/MPFR, VC++ runtime) must be on `PATH`. They are
bundled in the SDK's `bin/` directory.

**PowerShell:**

```powershell
$env:PATH += ";<path/to/slic3r_sdk>/bin"
$env:ORCA_RESOURCES = "<path/to/slic3r_sdk>/resources"
.\orca-slice-engine.exe model.3mf -v
```

**CMD:**

```cmd
set PATH=%PATH%;<path\to\slic3r_sdk>\bin
set ORCA_RESOURCES=<path\to\slic3r_sdk>\resources
orca-slice-engine.exe model.3mf -v
```

Alternatively, copy all DLLs from `slic3r_sdk/bin/` next to the exe so no
`PATH` setup is needed.

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

## Updating When OrcaSlicer Changes

```
1. Build new OrcaSlicer → new slic3r.dll
2. Run: ./scripts/package_sdk.sh <orca_root>
       → produces updated slic3r_sdk_x64_Release.zip
3. slic3r_c_api.h changed?
   ├─ Yes → cp new header to src/, rebuild consumer exe
   └─ No  → consumer is fine, just swap the SDK
4. Upload new zip (GitHub Releases / internal)
```

Most minor OrcaSlicer updates only need steps 1–2–4. The C API is designed to
be ABI-stable.
