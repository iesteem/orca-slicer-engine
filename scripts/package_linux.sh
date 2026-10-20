#!/bin/bash
# =============================================================================
# package_linux.sh — package orca-slice-engine for Linux deployment
#
# Usage:
#   ./scripts/package_linux.sh
#
# Output:
#   package/                 ← 可直接运行的部署目录
#   ├── orca-slice-engine    ← 二进制
#   └── resources/           ← Snapmaker profiles (来自 resources/ submodule; info/ 可选)
#
#   package-02.01.01.zip     ← 部署压缩包 (项目根目录)
#
# 使用:
#   unzip package-02.01.01.zip
#   cd package
#   export ORCA_RESOURCES="$PWD/resources"
#   ./orca-slice-engine -v model.3mf
# =============================================================================

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "  $*"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- locate sources ----
BIN="$PROJECT_DIR/build-static/orca-slice-engine"
[ -f "$BIN" ] || die "Binary not found: $BIN. Build with ORCA_STATIC=ON first."

# Presets come from the resources/ submodule (snapmaker-profiles, at resources/profiles)
SRC_RESOURCES="$PROJECT_DIR/resources/profiles"
[ -f "$SRC_RESOURCES/Snapmaker.json" ] || die "resources/profiles/Snapmaker.json not found (run: git submodule update --init)"
[ -d "$SRC_RESOURCES/Snapmaker" ] || die "resources/profiles/Snapmaker/ not found"

# ---- version (from engine's own CMakeLists.txt) ----
VERSION=$(grep 'ENGINE_VERSION' "$PROJECT_DIR/CMakeLists.txt" | head -1 | sed 's/.*"\(.*\)".*/\1/')
[ -n "${VERSION:-}" ] || VERSION=$("$BIN" --help 2>&1 | grep -oP 'v([\d.]+)' | head -1 | sed 's/^v//')
[ -n "${VERSION:-}" ] || die "Cannot determine version"
info "Version: $VERSION"

# ---- output paths ----
PKG_DIR="$PROJECT_DIR/package"
ZIP_FILE="$PROJECT_DIR/package-${VERSION}.zip"

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/resources"

# ---- build ----
echo "=== Packaging orca-slice-engine v${VERSION} for Linux ==="

# Binary
cp "$BIN" "$PKG_DIR/"
chmod 755 "$PKG_DIR/orca-slice-engine"
info "Binary: $(du -h "$BIN" | cut -f1)"

# Resources — profiles/ (Snapmaker vendor from submodule, JSON only)
echo ""
echo "=== Copying profiles/ (Snapmaker vendor from submodule, JSON only) ==="
mkdir -p "$PKG_DIR/resources/profiles"

cp "$SRC_RESOURCES/Snapmaker.json" "$PKG_DIR/resources/profiles/"
info "  Snapmaker.json"

cp -r "$SRC_RESOURCES/Snapmaker" "$PKG_DIR/resources/profiles/"
find "$PKG_DIR/resources/profiles/Snapmaker" -type f ! -name '*.json' -delete
find "$PKG_DIR/resources/profiles/Snapmaker" -type d -empty -delete 2>/dev/null || true

JSON_COUNT=$(find "$PKG_DIR/resources/profiles" -name '*.json' | wc -l)
PROFILES_SIZE=$(du -sh "$PKG_DIR/resources/profiles" | cut -f1)
info "  profiles: $PROFILES_SIZE ($JSON_COUNT JSON files)"

# Resources — info/ (optional, copied from an OrcaSlicer checkout when present;
# the engine itself does not read it)
ORCA_ROOT="${ORCA_ROOT:-$HOME/Desktop/code/OrcaSlicer}"
if [ -f "$ORCA_ROOT/resources/info/filament_info.json" ]; then
    echo ""
    echo "=== Copying info/ ==="
    cp -r "$ORCA_ROOT/resources/info" "$PKG_DIR/resources/"
    info "info: $(du -sh "$PKG_DIR/resources/info" | cut -f1)"
else
    info "info/ skipped (not found under ORCA_ROOT=$ORCA_ROOT)"
fi

RES_SIZE=$(du -sh "$PKG_DIR/resources" | cut -f1)
info "Total resources: $RES_SIZE"

# ---- summary ----
echo ""
echo "========================================"
echo " Package dir : $PKG_DIR"
PKG_SIZE=$(du -sh "$PKG_DIR" | cut -f1)
echo " Total size  : $PKG_SIZE"
echo ""

# ---- zip (项目根目录) ----
echo "=== Creating $ZIP_FILE ==="
rm -f "$ZIP_FILE"
(cd "$PROJECT_DIR" && zip -rq "$ZIP_FILE" "package")
ZIP_SIZE=$(du -sh "$ZIP_FILE" | cut -f1)
info "$(basename "$ZIP_FILE") ($ZIP_SIZE)"

# ---- done ----
echo ""
echo "=== Done ==="
echo ""
echo "Output:"
echo "  $PKG_DIR"
echo "  $ZIP_FILE"
echo ""
echo "Deploy:"
echo "  unzip package-${VERSION}.zip"
echo "  cd package"
echo "  export ORCA_RESOURCES=\$PWD/resources"
echo "  ./orca-slice-engine -v model.3mf"
echo ""
echo "System dependencies (usually pre-installed):"
echo "  apt: libstdc++6 libexpat1 libfontconfig1 zlib1g"
