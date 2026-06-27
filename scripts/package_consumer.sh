#!/usr/bin/env bash
set -eo pipefail

# orca-slice-engine consumer 模式打包脚本
# 用法: ./package_consumer.sh [OrcaSlicer_dir]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-dll"
ORCASLICER_DIR="${1:-/home/joyx/Desktop/code/OrcaSlicer}"
PKG_DIR="${PROJECT_DIR}/package"

echo "=== Packaging orca-slice-engine (consumer mode) ==="

# 清理旧包
rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}"/{bin,lib,resources/{profiles,printers,info}}

# 1. 拷贝二进制
echo "[1/3] Copying binary..."
cp -f "${BUILD_DIR}/orca-slice-engine" "${PKG_DIR}/bin/"
echo "  bin/orca-slice-engine"

# 2. 拷贝共享库
echo "[2/3] Copying shared library..."
# Detect actual .so version file (matches current CMake VERSION)
SO_FILE=$(ls "${BUILD_DIR}/libslic3r.so."*.*.* 2>/dev/null | sort -V | tail -1)
cp -f "${SO_FILE}" "${PKG_DIR}/lib/"
SO_BASENAME=$(basename "${SO_FILE}")
ln -sf "${SO_BASENAME}" "${PKG_DIR}/lib/libslic3r.so.2"
ln -sf libslic3r.so.2      "${PKG_DIR}/lib/libslic3r.so"
echo "  lib/${SO_BASENAME}"

# 3. 拷贝资源（仅引擎必需的 vendor）
echo "[3/3] Copying resources (Snapmaker + OrcaFilamentLibrary)..."
for vendor in Snapmaker OrcaFilamentLibrary; do
    cp "${ORCASLICER_DIR}/resources/profiles/${vendor}.json" "${PKG_DIR}/resources/profiles/"
    cp -r "${ORCASLICER_DIR}/resources/profiles/${vendor}" "${PKG_DIR}/resources/profiles/"
    echo "  profiles/${vendor}"
done
cp "${ORCASLICER_DIR}/resources/profiles/hotend.stl" "${PKG_DIR}/resources/profiles/"
echo "  profiles/hotend.stl"

cp -r "${ORCASLICER_DIR}/resources/printers/"* "${PKG_DIR}/resources/printers/"
echo "  printers/"

cp -r "${ORCASLICER_DIR}/resources/info/"* "${PKG_DIR}/resources/info/"
echo "  info/"

# 统计
EXE_SIZE=$(du -h "${PKG_DIR}/bin/orca-slice-engine" | cut -f1)
SO_SIZE=$(du -h "${PKG_DIR}/lib/${SO_BASENAME}" | cut -f1)
TOTAL_SIZE=$(du -sh "${PKG_DIR}" | cut -f1)

echo ""
echo "====================================================="
echo "  PACKAGE SUCCESS"
echo "  ${PKG_DIR}"
echo "    bin/orca-slice-engine  ${EXE_SIZE}"
echo "    lib/libslic3r.so       ${SO_SIZE}"
echo "    total                  ${TOTAL_SIZE}"
echo ""
echo "  Usage: LD_LIBRARY_PATH=${PKG_DIR}/lib ${PKG_DIR}/bin/orca-slice-engine -r ${PKG_DIR}/resources <input.3mf>"
echo "====================================================="
