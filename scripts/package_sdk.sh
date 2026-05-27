#!/bin/bash
# =============================================================================
# package_sdk.sh — package slic3r SDK from an OrcaSlicer build tree
#
# Usage:
#   ./scripts/package_sdk.sh <orca_root> [output_dir]
#
# Example:
#   ./scripts/package_sdk.sh /c/code/iesteem-build-OrcaSlicer ./slic3r_sdk
#
# Output structure:
#   slic3r_sdk/
#   ├── lib/slic3r.lib          ← import library (compile-time)
#   ├── bin/slic3r.dll          ← core DLL
#   ├── bin/TK*.dll             ← OCCT DLLs (full set, ~38 files)
#   ├── bin/libgmp-10.dll       ← GMP/MPFR math libs
#   ├── bin/libmpfr-4.dll
#   ├── bin/msvcp140.dll        ← VC++ runtime
#   ├── bin/vcruntime140.dll
#   ├── bin/vcruntime140_1.dll
#   └── resources/              ← printer/filament/process presets
#
# The consumer builds with:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=../slic3r_sdk
#
# Requires: bash, pwsh (for zip)
# =============================================================================

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

# ---- args ----
ORCA_ROOT="${1:?Usage: $0 <orca_root> [output_dir]}"
OUTPUT_DIR="${2:-./slic3r_sdk}"
ZIP_NAME="slic3r_sdk_x64_Release.zip"

# Resolve to absolute
ORCA_ROOT="$(cd "$ORCA_ROOT" 2>/dev/null && pwd || echo "$ORCA_ROOT")"
OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" 2>/dev/null && pwd || pwd)"
OUTPUT_DIR="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"

# ---- source paths ----
SLIC3R_DLL="$ORCA_ROOT/build/src/libslic3r/Release/slic3r.dll"
SLIC3R_LIB="$ORCA_ROOT/build/src/libslic3r/Release/slic3r.lib"
OCCT_DIR="$ORCA_ROOT/deps/build/OrcaSlicer_dep/usr/local/bin/occt"
DEPS_BIN="$ORCA_ROOT/deps/build/OrcaSlicer_dep/usr/local/bin"
RESOURCES="$ORCA_ROOT/resources"

# ---- verify ----
echo "=== Verifying sources ==="
for f in "$SLIC3R_DLL" "$SLIC3R_LIB"; do
    [ -f "$f" ] || die "$f not found. Is OrcaSlicer built? (Release config required)"
    info "Found: $(basename "$f")"
done
[ -d "$OCCT_DIR" ] || die "OCCT dir not found: $OCCT_DIR"
[ -d "$RESOURCES" ] || die "Resources dir not found: $RESOURCES"

# ---- build output ----
echo ""
echo "=== Building SDK at $OUTPUT_DIR ==="
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"/{lib,bin}

# Import library
cp "$SLIC3R_LIB" "$OUTPUT_DIR/lib/"
info "slic3r.lib"

# Core DLL
cp "$SLIC3R_DLL" "$OUTPUT_DIR/bin/"
info "slic3r.dll"

# OCCT DLLs — copy all, avoid fragile transitive tracing
echo ""
echo "=== OCCT DLLs ==="
OCCT_COUNT=0
for dll in "$OCCT_DIR"/TK*.dll; do
    [ -f "$dll" ] || continue
    cp "$dll" "$OUTPUT_DIR/bin/"
    OCCT_COUNT=$((OCCT_COUNT + 1))
done
info "$OCCT_COUNT TK*.dll copied"

# Math libraries
echo ""
echo "=== Math libraries ==="
for dll in libgmp-10.dll libmpfr-4.dll; do
    [ -f "$DEPS_BIN/$dll" ] || { info "WARNING: $dll not found"; continue; }
    cp "$DEPS_BIN/$dll" "$OUTPUT_DIR/bin/"
    info "$dll"
done

# MSVC runtime
echo ""
echo "=== VC++ runtime ==="
for dll in msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
    [ -f "$DEPS_BIN/$dll" ] || { info "WARNING: $dll not found (install VC++ Redist)"; continue; }
    cp "$DEPS_BIN/$dll" "$OUTPUT_DIR/bin/"
    info "$dll"
done

# Resources
echo ""
echo "=== Copying resources/ (this may take a minute) ==="
cp -r "$RESOURCES" "$OUTPUT_DIR/resources"
RES_SIZE=$(du -sh "$OUTPUT_DIR/resources" 2>/dev/null | cut -f1 || echo "?")
info "resources: $RES_SIZE"

# ---- summary ----
echo ""
echo "========================================"
echo "SDK assembled : $OUTPUT_DIR"
BIN_COUNT=$(ls "$OUTPUT_DIR/bin"/*.dll 2>/dev/null | wc -l || echo 0)
SDK_SIZE=$(du -sh "$OUTPUT_DIR" 2>/dev/null | cut -f1 || echo "?")
echo "DLLs          : $BIN_COUNT"
echo "Total size    : $SDK_SIZE"
echo ""

# ---- zip ----
echo "=== Creating $ZIP_NAME ==="
ZIP_DEST="$OUTPUT_PARENT/$ZIP_NAME"

# Convert MSYS paths to Windows format for PowerShell
win_output_dir() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
WIN_SDK_DIR=$(win_output_dir "$OUTPUT_DIR")
WIN_ZIP_DEST=$(win_output_dir "$ZIP_DEST")

if command -v pwsh &>/dev/null; then
    pwsh -NoProfile -Command "
        Compress-Archive -Path '$WIN_SDK_DIR' -DestinationPath '$WIN_ZIP_DEST' -Force
    "
    info "Package: $ZIP_DEST"
    info "Size:    $(du -sh "$ZIP_DEST" 2>/dev/null | cut -f1 || echo '?')"
elif command -v zip &>/dev/null; then
    (cd "$OUTPUT_PARENT" && zip -r "$ZIP_NAME" "$(basename "$OUTPUT_DIR")")
    info "Package: $ZIP_DEST"
else
    info "No pwsh/zip found. SDK at $OUTPUT_DIR (uncompressed)"
fi

echo ""
echo "=== Done ==="
echo ""
echo "Consumer usage:"
echo "  export CMAKE_PREFIX_PATH=$OUTPUT_DIR"
echo "  cmake -S . -B build"
echo "  cmake --build build --config Release"
echo ""
echo "Runtime: add $OUTPUT_DIR/bin to PATH, or copy DLLs next to the exe."
