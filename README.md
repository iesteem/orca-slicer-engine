# orca-slice-engine

Static slicing engine. Compiles `main.c` (C CLI frontend) together with the engine C++ sources into a single `orca-slice-engine` binary that **statically links libslic3r + all dependencies** (libslic3r, CGAL, OCCT, Boost, OpenCV, TBB) — no `.so`/`.dll` at runtime. Linux only.

## Changelog

> **Built version**: `02.01.06` — defined by `ENGINE_VERSION` in `CMakeLists.txt`.
> This is the single source of truth; the tests and `scripts/package_linux.sh` read the
> version from CMake. The entries below track the broader product release line.

### v02.01.11 (2026-06-12)

- **计划新增 `--threads <N>` 参数（尚未实现）**：计划限制 TBB 并行线程数，用于避免资源耗尽，
  拟通过 `tbb::global_control::max_allowed_parallelism` 实现，默认 0 表示使用全部核心。
  (ebcae02, 219b4f7) — 注：截至当前 `src/` 中尚未实现该参数（见缺口报告 G3）。

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
orca-slice-engine (single static binary, Linux)
  ├── main.c                 ← C CLI frontend (pure C, #include "slic3r_c_api.h")
  ├── slic3r_c_api.{h,cpp}   ← ABI boundary: 7 pure-C exports, zero C++ type leakage
  ├── SliceEngine.cpp + engine sources (JsonReport / GeometryCheck / Utils / PresetRollback / nanosvg)
  └── statically linked: libslic3r.a + CGAL + OCCT + Boost + OpenCV + TBB
```

## Prerequisites

- CMake ≥ 3.13
- A C11 + C++17 toolchain (GCC or Clang) on **Linux**
- A fully built **OrcaSlicer** tree (`ORCA_ROOT`). This project does not vendor
  libslic3r; it links `libslic3r.a` and the other static archives extracted from
  an OrcaSlicer build. The `resources/` preset directory is also taken from there.

## Obtaining the static libraries

`scripts/gen_static_libs.sh` inspects an existing OrcaSlicer build and writes
`cmake/StaticConfig.cmake`, which defines the authoritative library list
(`ORCA_STATIC_LIBS`), include directories (`ORCA_STATIC_INCLUDES`) and compile
defines (`ORCA_STATIC_DEFINES`):

```bash
ORCA_ROOT=/path/to/OrcaSlicer scripts/gen_static_libs.sh
```

You also need the preset `resources/` directory available at runtime (set via
`ORCA_RESOURCES` or `-r`); the packaging step copies it to `package/resources`.

## Build (ORCA_STATIC — the only mode)

```bash
git clone https://github.com/iesteem/orca-slice-engine.git
cd orca-slice-engine

# 1. Generate cmake/StaticConfig.cmake from a built OrcaSlicer tree
ORCA_ROOT=/path/to/OrcaSlicer scripts/gen_static_libs.sh

# 2. Configure + build the single static binary
cmake -S . -B build-static -DORCA_STATIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-static -j
```

Output: `build-static/orca-slice-engine` — a single static executable with no
runtime `.so`/`.dll` dependencies beyond the system libc.

## Run

```bash
export ORCA_RESOURCES=/path/to/OrcaSlicer/resources   # or pass -r <dir>
./build-static/orca-slice-engine model.3mf -v
```

The preset `resources/` directory must be available at runtime (via
`ORCA_RESOURCES` or `-r`).

## CLI Reference

```
orca-slice-engine <input.3mf> [options]

  -o, --output <path>   Output path (without extension, default: derived from input)
  -p, --plate <id>      Plate ID (0 = all plates, default: 0)
  -f, --format <fmt>    Output format: gcode | gcode.3mf (default: gcode.3mf)
  -r, --resources <dir> Resources directory (or set ORCA_RESOURCES env var)
  -v, --verbose         Verbose output
  -h, --help            Show this help
```

## Updating when OrcaSlicer changes

This project **statically links** libslic3r, so any OrcaSlicer change requires
regenerating the static library config and rebuilding from source — there is no
runtime DLL to "swap":

```
┌────────────────────────────────────────────┐
│  your repo (main.c + engine C++ sources)    │
│  depends on: slic3r_c_api.h (7 C functions) │
├────────────────────────────────────────────┤
│  slic3r_c_api.h   ← ABI boundary           │  ← if stable, no recompile needed
├────────────────────────────────────────────┤
│  libslic3r.a + CGAL + OCCT + Boost (static) │  ← extracted from OrcaSlicer
└────────────────────────────────────────────┘
```

### What changes and what to do

| Upstream change | Impact | Action |
|----------------|--------|--------|
| libslic3r internal implementation | Rebuild needed | Regenerate StaticConfig, rebuild |
| libslic3r header API changes | SliceEngine.cpp may break | Update engine source, rebuild |
| `slic3r_c_api.h` signature changes | Callers need recompile | Update header, rebuild |
| `resources/` format changes | Runtime may fail | Replace resources directory |

### Update workflow

```
1. Rebuild OrcaSlicer -> new libslic3r.a + deps
2. ORCA_ROOT=<new OrcaSlicer> scripts/gen_static_libs.sh
3. cmake --build build-static -j
4. slic3r_c_api.h changed?
   - Yes -> cp new header to src/, rebuild
   - No  -> engine is fine, just relink
```

The C API is designed to be ABI-stable, so most minor updates only need steps 1–3.

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

## Build modes

This project has a **single build mode: ORCA_STATIC** — a single static
executable that statically links libslic3r and all dependencies. See
[Build](#build-orca_static--the-only-mode) above for the exact commands. The
previous Consumer mode (linking a prebuilt `slic3r.dll`) has been removed.
