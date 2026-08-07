# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
within the `MAJOR.MINOR.PATCH` engine version scheme (`02.01.xx`).

## [v02.01.04] — PR #6

### Fixed
- **Removed filament trim; wipe-tower delegated to libslic3r.** The engine no
  longer calls `resize()` on the filament list — `ToolOrdering` in libslic3r
  owns the decision. Fixes a bug where `trim` without remap collapsed JSON
  statistics to slot-0 only, and avoids an `append_tcr` crash from a misjudged
  `has_wipe_tower`. (`02a4135`)
- **Count modifier-volume extruders** when deciding single-color trim, so a
  multi-color model with modifiers is no longer mis-detected as single-color
  and wrongly trimmed. (`a789686`)
- **Resolve `?:` type mismatch** in `assemble_plate_stats`. (`e5beb87`)

### Changed
- **Snapshot post-trim filament colour / type / nozzle / diameter / density**
  so JSON statistics match the slots actually used at print time. (`862b753`)
- **Decouple SEMM skip-trim from `used_extruders` stats set** — the two
  concerns are now tracked independently to avoid cross-contamination.
  (`272c553`)
- **Dropped duplicated post-trim snapshot block** in `export_gcode`. (`48069ee`)

## [v02.01.03] — PR #2 + PR #3

### Added
- **`inherits_group` resolution.** Parses the 3MF inheritance chain as
  `[process, fil_0..N-1, printer]` (N+2 elements), so keys like `fil_0` are
  recognised correctly. (`2febb62`)
- **Per-plate temperature-mixing check + correct slot retention on trim**,
  mirroring the desktop `check_filament_temp_mixing` behaviour — trim now
  keeps the correct slot rather than blindly resizing. (`17362fd`)
- **`--skip-preset-substitution`** CLI flag to bypass official preset
  enforcement. (`70f7225`)
- **Organic-supports and variable-layer-height detection** now raises an
  error (previously silent), migrated from string match to enum type.
  (`8f76a53`, `e2fb935`)

### Fixed
- **Thumbnail passthrough.** Thumbnails are read from disk during decode,
  fixing missing `; thumbnail` headers in emitted G-code. (`5399300`,
  `c3b3530`, `dde0ae8`)
- **`TOOLPATH_CONFLICT`** now uses `EXIT_POSTPROCESS_ERROR (7)` and sets
  `m_any_error` to block `gcode.3mf` generation. (`b1d7aae`, `f25b8d4`)
- **`PRINT_BY_OBJECT_CAUTION`** promoted from warning to error and now
  blocks `gcode.3mf` generation; message updated to English and references
  Snapmaker Orca. (`24a4f2e`, `275154e`, `8dedd3d`)
- **`PRESET_PRINTER_NOT_FOUND`** demoted to warning — there is a downstream
  fallback. (`cb9bae6`)
- Removed `fprintf(stderr)` calls that violated the coding standard. (`7b9f6bf`)

### Changed
- **God-class decomposition of `SliceEngine`.** `load_3mf` split into
  validate / read / sanitize; `apply_*_official_preset` extracted as
  dedicated helpers; `validate_presets` renamed to `collect_config_warnings`.
  Every function previously over 150 lines has been split. ~1790 lines of
  dead code (`PlateProcessor`, `StatisticsBuilder`) removed. (`c9fae36`,
  `927a19e`, `1bf94cf`, `ffb8e0c`, `2798813`, `29d0d4a`, etc.)
- **Unified preset-substitution reporting** across printer / filament /
  process. `PRINTER_SUBSTITUTED` now preserves the user's original preset
  name. (`e3167da`, `5d283d5`)
- **Severity clean-up.** `SPIRAL_LIFT_NEAR_BOUNDARY` demoted to warning
  (no longer blocks slicing with early return). (`343fa4b`, `043c1ec`)
- **Snapmaker-only packaging.** Third-party packages stripped; `--log-file`
  removed in favour of unified `--log`. (`70f7225`)

## [v02.01.01] — PR #1

### Added
- **Decoupled `ENGINE_VERSION`** from OrcaSlicer's version string.
  (`2fb040b`)
- **Linux packaging script** `scripts/package_linux.sh`. (`8b31045`)
- **`-j` / `--json` CLI option** for standalone slice-statistics JSON output.
  (`ac997a1`)
- **`--log` log-file output** with sensible default path derived from the
  output directory; supports both level mapping and a `--log-file` alias
  for backward compatibility. (`8ff0eb8`, `254652b`, `fc38b95`, `2f826b8`)
- **Model ensure-on-bed**: imported models are auto-positioned onto the bed.
  (`55490e9`, `e14cc26`)
- **Official U1 printer preset replacement** before slicing, so user
  printer config is overridden wholesale. (`7802c66`)
- **Official process preset application**, fixing brim / skirt mismatch
  with desktop. (`48d6e15`)
- **`ORCA_STATIC` static monolithic build mode.** (`bc6629e`)

### Fixed
- **Case-insensitive `.3mf` extension detection.** (`4268305`)
- **Z lift dropped by `Print::apply`** during ensure-on-bed, which truncated
  the model. (`98ea2b1`)
- **`--log` argument-parsing defect** that overwrote `input_3mf`; info-level
  log mapping; auto-path derivation now prefers the output directory over
  the `input_3mf` directory. (`254652b`, `fc38b95`, `2f826b8`)
- **Nozzle-suffix detection** migrated from fragile substring match to an
  independent token. (`87af1e6`)
- **`filament_roll_back`** no longer uses `orca_filament`. (`04e2e8c`)

### Changed
- **Filament-rollback strategy**: non-official filaments fall back to a
  base category, with unified full overwrite for printer/filament
  substitution. (`b34a914`, `67969ee`)
- **Unified JSON output to the `JsonReport` schema.** (`bc6629e`)
- **CLI argument handling aligned** between `main.c` and the C API.
  (`dc3f09f`)

---

Versions prior to v02.01.01 tracked OrcaSlicer's version directly and are
not listed here.
