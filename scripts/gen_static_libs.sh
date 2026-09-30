#!/usr/bin/env bash
#
# gen_static_libs.sh — 从 OrcaSlicer 的 build.ninja 提取 libslic3r 静态链接清单,
# 生成 cmake/StaticLibs.cmake(定义 ORCA_STATIC_LIBS),供 ORCA_STATIC 构建模式使用。
#
# 权威来源:OrcaSlicer build-engine 已成功链接出的 `src/snapmaker-orca` 可执行文件。
# 它的 LINK_LIBRARIES 是「libslic3r + 全部依赖」的拓扑有序、已验证清单。
# 我们原样复用该顺序(含其中故意的重复,用于解决静态库循环依赖)。
#
# 用法:
#   ORCA_ROOT=/path/to/OrcaSlicer scripts/gen_static_libs.sh
# 或:
#   scripts/gen_static_libs.sh /path/to/OrcaSlicer
#
set -euo pipefail

ORCA_ROOT="${1:-${ORCA_ROOT:-}}"
if [ -z "$ORCA_ROOT" ]; then
    echo "ERROR: 需指定 OrcaSlicer 根目录。用法: ORCA_ROOT=<path> $0  (或作为第一个参数)" >&2
    exit 1
fi

ORCA_ROOT="$(cd "$ORCA_ROOT" && pwd)"          # 规范化为绝对路径
BUILD_DIR="$ORCA_ROOT/build-engine"
NINJA="$BUILD_DIR/build.ninja"

if [ ! -f "$NINJA" ]; then
    echo "ERROR: 找不到 $NINJA。请先在 OrcaSlicer 完成 build-engine 构建。" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/cmake"
OUT="$OUT_DIR/StaticConfig.cmake"
mkdir -p "$OUT_DIR"

# 提取 snapmaker-orca 链接规则的 LINK_LIBRARIES 行(从 build 行起,到首个 LINK_LIBRARIES = 止)
RAW="$(awk '
    /^build src\/snapmaker-orca: CXX_EXECUTABLE_LINKER/ {f=1}
    f && /^  LINK_LIBRARIES =/ {sub(/^  LINK_LIBRARIES = /,""); print; exit}
' "$NINJA")"

if [ -z "$RAW" ]; then
    echo "ERROR: 在 $NINJA 中未找到 snapmaker-orca 的 LINK_LIBRARIES。" >&2
    exit 1
fi

# 逐 token 处理:
#   - 相对路径(build-engine 下,如 src/.../x.a、deps_src/.../x.a、lib/x.a)→ 绝对化到 $BUILD_DIR
#   - ../deps/... → 绝对化(相对 build-engine)
#   - 绝对路径(/usr/lib/.../x.so 等)→ 原样
#   - 系统库标志(-ldl -lm -lpthread ...)→ 原样
emit() {
    local tok="$1"
    case "$tok" in
        -*)                                  # 链接器标志 / -l 系统库
            printf '  "%s"\n' "$tok" ;;
        /*)                                  # 绝对路径
            printf '  "%s"\n' "$tok" ;;
        *)                                   # build-engine 相对路径
            printf '  "%s"\n' "$(cd "$BUILD_DIR" && cd "$(dirname "$tok")" && pwd)/$(basename "$tok")" ;;
    esac
}

# --- 提取 libslic3r 的 DEFINES 与 INCLUDES(编译引擎 C++ 源时需与 libslic3r 一致) ---
# 取任一 libslic3r TU 的编译规则变量块。
DEF_RAW="$(awk '
    /libslic3r\.dir\/Print\.cpp\.o:/ {f=1}
    f && /^  DEFINES =/ {sub(/^  DEFINES = /,""); print; exit}
' "$NINJA")"

INC_RAW="$(awk '
    /libslic3r\.dir\/Print\.cpp\.o:/ {f=1}
    f && /^  INCLUDES =/ {sub(/^  INCLUDES = /,""); print; exit}
' "$NINJA")"

{
    echo "# 自动生成 — 请勿手工编辑。由 scripts/gen_static_libs.sh 从以下来源提取:"
    echo "#   $NINJA"
    echo "#   target: src/snapmaker-orca (LINK_LIBRARIES) + libslic3r (DEFINES/INCLUDES)"
    echo "# ORCA_ROOT = $ORCA_ROOT"
    echo "# 顺序原样保留(含故意重复,用于解决静态库循环依赖)。"
    echo ""
    echo "set(ORCA_STATIC_LIBS"
    for tok in $RAW; do
        [ -n "$tok" ] && emit "$tok"
    done
    echo ")"

    echo ""
    echo "# libslic3r 编译期宏(去掉 -D 前缀,供 target_compile_definitions)"
    echo "set(ORCA_STATIC_DEFINES"
    for tok in $DEF_RAW; do
        case "$tok" in
            -D*) printf '  "%s"\n' "${tok#-D}" ;;
        esac
    done
    echo ")"

    echo ""
    echo "# libslic3r include 目录(-I 与 -isystem 统一为目录列表)"
    echo "set(ORCA_STATIC_INCLUDES"
    # INCLUDES 形如:-I<dir> -I<dir> -isystem <dir> -isystem <dir>
    # 用 read 逐 token,遇 -isystem 取下一个 token,遇 -I<dir> 去前缀。
    set -- $INC_RAW
    while [ $# -gt 0 ]; do
        case "$1" in
            -isystem) shift; [ $# -gt 0 ] && printf '  "%s"\n' "$1" ;;
            -I?*)     printf '  "%s"\n' "${1#-I}" ;;
        esac
        shift
    done
    # headless 专属:libslic3r TU 列表里没有(GUI 才用),但我们的 nanosvg.cpp 需要
    for extra in deps_src/nanosvg; do
        d="$ORCA_ROOT/$extra"
        [ -d "$d" ] && printf '  "%s"\n' "$d"
    done
    echo ")"
} > "$OUT"

NLIBS="$(grep -c '\.a"\|^  "-l\|\.so"' "$OUT" || true)"
echo "已生成 $OUT"
echo "  链接项: ${NLIBS}   defines: $(echo "$DEF_RAW" | grep -oc '\-D' || true)"
