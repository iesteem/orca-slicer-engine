# orca-slice-engine

Pure C consumer of `slic3r.dll` — no C++ dependencies, no libslic3r headers.

## Changelog

### v02.01.11 (2026-06-12)

- **新增 `--threads <N>` 参数**：限制 TBB 并行线程数，用于避免资源耗尽。
  通过 `tbb::global_control::max_allowed_parallelism` 实现，默认 0 表示使用全部核心。
  (ebcae02, 219b4f7)

- **新增 G-code 导出内存风险评估**：引入逐层评分机制，在执行 G-code 导出前评估内存风险。
  优化 `model_complexity.py`、`batch_benchmark.py`、`3mf_score.py` 工具脚本，
  基于 k3s 切片验证数据重新校准 `memory_risk` 阈值。
  (34a3373, fe6526b)

### v02.01.10 (2026-06-11)

- **JSON 字段重命名**：`plates[].filaments[].filament_id` → `plates[].filaments[].id`，
  同时将 `SliceOutputStats::FilamentDetail::filament_id` 成员变量同步改为 `id`。
  注意：libslic3r 的 `FilamentInfo::filament_id`（`std::string` 类型）保持不变。
  (fca9ee3, f685137)

- **基础版本**：UTF-8 BOM、C++ 风格转换、安全函数替换等代码规范修复。
  (1e6a69c, 8fccaef)

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

## Prerequisites

- CMake ≥ 3.13
- MSVC 2022 (or any C11 compiler)
- **slic3r SDK** — a self-contained bundle of `slic3r.dll`, `slic3r.lib`, and all runtime dependencies. You do NOT need a full OrcaSlicer source tree.

## Getting the slic3r SDK

### Option A: Download pre-built (easiest)

Download `slic3r_sdk_x64_Release.zip` from GitHub Releases (or your internal file server) and extract it.

### Option B: Package from an existing OrcaSlicer build

If you have a fully built OrcaSlicer tree, run the packaging script:

```bash
# From the orca-slice-engine repo root:
./scripts/package_sdk.sh /path/to/OrcaSlicer
```

This traces all DLL dependencies, copies the `resources/` preset directory,
and produces `slic3r_sdk_x64_Release.zip` (~119 MB).

**Prerequisites for the script:** bash (MSYS2 / git-bash) with `objdump`, and PowerShell 7+ for the zip step.

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

## Build

```bash
git clone https://github.com/iesteem/orca-slice-engine.git
cd orca-slice-engine

# Point CMAKE_PREFIX_PATH to the extracted SDK directory
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH="<path/to/slic3r_sdk>" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

Output: `build/Release/orca-slice-engine.exe` (~14 KB).

No libslic3r. No CGAL. No OCCT. No Boost. Just one `.lib` at compile time.

## Run

All runtime DLLs (OCCT, GMP/MPFR, VC++ runtime) must be on `PATH`.
They are bundled in the SDK's `bin/` directory.

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

Alternatively, copy all DLLs from `slic3r_sdk/bin/` next to the exe so no PATH setup is needed.

## CLI Reference

```
orca-slice-engine.exe <input.3mf> [options]

  -o, --output <path>   Output path (without extension, default: derived from input)
  -p, --plate <id>      Plate ID (0 = all plates, default: 0)
  -f, --format <fmt>    Output format: gcode | gcode.3mf (default: gcode.3mf)
  -r, --resources <dir> Resources directory (or set ORCA_RESOURCES env var)
  -v, --verbose         Verbose output
  -h, --help            Show this help
```

## Updating when OrcaSlicer changes

The architecture is layered to minimize churn:

```
┌────────────────────────────────────────────┐
│  main.c  (pure C consumer)                 │  ← your repo
│  depends on: slic3r_c_api.h (7 C functions)│
├────────────────────────────────────────────┤
│  slic3r_c_api.h   ← ABI boundary          │  ← if stable, no consumer change
├────────────────────────────────────────────┤
│  slic3r.dll                               │
│  ├── SliceEngine.cpp  ← uses libslic3r     │  ← your repo (DLL source)
│  └── JsonReport.cpp / GeometryCheck.cpp   │
├────────────────────────────────────────────┤
│  libslic3r + CGAL + OCCT + Boost          │  ← upstream OrcaSlicer
└────────────────────────────────────────────┘
```

### What changes and what to do

| Upstream change | Impact | Action |
|----------------|--------|--------|
| libslic3r internal implementation | None | Swap DLL, done |
| libslic3r header API changes | SliceEngine.cpp may break | Update engine source, rebuild DLL |
| `slic3r_c_api.h` signature changes | Consumer needs recompile | Update header, rebuild exe |
| `resources/` format changes | Runtime may fail | Replace resources directory |

### Update workflow

```
1. Build new OrcaSlicer → new slic3r.dll
2. Run: ./scripts/package_sdk.sh <orca_root>
       → produces updated slic3r_sdk_x64_Release.zip
3. slic3r_c_api.h changed?
   ├─ Yes → cp new header to src/, rebuild consumer exe
   └─ No  → consumer is fine, just swap the SDK
4. Upload new zip (GitHub Releases / internal)
```

Most minor OrcaSlicer updates only need steps 1–2–4. The C API is designed to be ABI-stable.

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

## Two Build Modes

The CMakeLists.txt supports two modes:

### Consumer mode (default)

Builds only `orca-slice-engine.exe` against a prebuilt `slic3r.dll`:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdk_dir>
```

### DLL mode (opt-in)

Builds `slic3r.dll` from engine source against a full libslic3r SDK:

```bash
cmake -S . -B build -DBUILD_SLIC3R_DLL=ON -DCMAKE_PREFIX_PATH=<OrcaSlicer SDK dir>
```

This requires the complete OrcaSlicer dependency chain (libslic3r, CGAL, OCCT, Boost, TBB, OpenSSL). Use this when you need to iterate on the DLL internals.
