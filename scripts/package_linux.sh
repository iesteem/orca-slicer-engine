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
#   └── resources/           ← info + Snapmaker profiles
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

ORCA_ROOT="${ORCA_ROOT:-$HOME/Desktop/code/OrcaSlicer}"
SRC_RESOURCES="$ORCA_ROOT/resources"
[ -d "$SRC_RESOURCES" ] || die "OrcaSlicer resources not found: $SRC_RESOURCES (set ORCA_ROOT env var)"
[ -d "$SRC_RESOURCES/profiles/Snapmaker" ] || die "profiles/Snapmaker/ not found"
[ -f "$SRC_RESOURCES/profiles/Snapmaker.json" ] || die "profiles/Snapmaker.json not found"
[ -f "$SRC_RESOURCES/info/filament_info.json" ] || die "info/filament_info.json not found"

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

# Resources — info/
echo ""
echo "=== Copying info/ ==="
cp -r "$SRC_RESOURCES/info" "$PKG_DIR/resources/"
info "info: $(du -sh "$PKG_DIR/resources/info" | cut -f1)"

# Resources — profiles/ (Snapmaker only, JSON only)
echo ""
echo "=== Copying profiles/ (Snapmaker vendor, JSON only) ==="
mkdir -p "$PKG_DIR/resources/profiles"

cp "$SRC_RESOURCES/profiles/Snapmaker.json" "$PKG_DIR/resources/profiles/"
info "  Snapmaker.json"

cp -r "$SRC_RESOURCES/profiles/Snapmaker" "$PKG_DIR/resources/profiles/"
find "$PKG_DIR/resources/profiles/Snapmaker" -type f ! -name '*.json' -delete
find "$PKG_DIR/resources/profiles/Snapmaker" -type d -empty -delete 2>/dev/null || true

JSON_COUNT=$(find "$PKG_DIR/resources/profiles" -name '*.json' | wc -l)
PROFILES_SIZE=$(du -sh "$PKG_DIR/resources/profiles" | cut -f1)
info "  profiles: $PROFILES_SIZE ($JSON_COUNT JSON files)"

# ---- bundle self-check (C5 出厂门, 2026-09-20) ----
# 校验对象 = 打进本产物的 bundle（非 fork 源头）。所有已实例化 filament 预设
# 沿 inherits 链合并后必须解析出 filament_vendor，且链不得悬空/成环。
# 背景：引擎侧 system preset 加载失败是整单致命、无降级（判例 P-09），而
# bundle 自 25ac1b6 起不进 git 跟踪、打包时从 fork 检出实时拷贝（无锁定），
# 唯一可靠拦截点是打包时刻——2026-09-18 云审查 C5 终审 cannot_verify 的
# 闭环手段。缺 @U1 base 层 = U1 预设链悬空，由 dangling 分支捕获。
command -v python3 >/dev/null 2>&1 || die "Bundle self-check requires python3"
FILAMENT_DIR="$PKG_DIR/resources/profiles/Snapmaker/filament"
[ -d "$FILAMENT_DIR" ] || die "Bundle self-check: $FILAMENT_DIR missing — bundle layout changed?"
python3 - "$FILAMENT_DIR" <<'PYEOF' || die "Bundle self-check FAILED (filament_vendor chain)"
import json, glob, os, sys

d = sys.argv[1]
by_name, bad_json = {}, []
for f in sorted(glob.glob(os.path.join(d, "*.json"))):
    try:
        with open(f, encoding="utf-8") as fh:
            j = json.load(fh)
    except Exception as e:
        bad_json.append(f"{os.path.basename(f)}: {e}")
        continue
    if isinstance(j, dict) and j.get("name"):
        by_name[j["name"]] = j

errors = [f"unparseable JSON: {b}" for b in bad_json]
dangling, missing_vendor, inst = [], [], 0
for name, j in by_name.items():
    if j.get("instantiation") != "true":
        continue
    inst += 1
    seen, cur, vendor = set(), j, None
    while isinstance(cur, dict):
        if cur.get("filament_vendor"):
            vendor = cur["filament_vendor"]
            break
        inh = cur.get("inherits")
        if not inh:
            break
        if inh in seen:
            errors.append(f"circular inherits at '{name}' ({inh})")
            break
        seen.add(inh)
        cur = by_name.get(inh)
        if cur is None:
            dangling.append(f"{name} -> {inh}")
            break
    if not vendor:
        missing_vendor.append(name)

errors += [f"dangling inherits chain: {n}" for n in dangling]
errors += [f"no filament_vendor after chain merge: {n}" for n in missing_vendor]

if errors:
    print(f"  self-check: {inst} instantiated, {len(dangling)} dangling, "
          f"{len(missing_vendor)} missing vendor", file=sys.stderr)
    for e in errors[:20]:
        print(f"    FAIL {e}", file=sys.stderr)
    if len(errors) > 20:
        print(f"    ... and {len(errors) - 20} more", file=sys.stderr)
    sys.exit(1)

# Empty/absent bundle must not pass silently (copy step failure would
# otherwise yield a vacuous green: 0 presets, 0 errors).
if inst == 0:
    print("    FAIL no instantiated presets found — bundle empty or layout changed", file=sys.stderr)
    sys.exit(1)

print(f"  bundle self-check: {len(by_name)} presets, {inst} instantiated, "
      f"0 dangling, 0 missing filament_vendor — OK")
PYEOF

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
