#!/usr/bin/env bash
set -eo pipefail

# orca-slice-engine: build + package in one step
# Usage: ./scripts/build.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== [1/3] Building consumer mode ==="
cd "$PROJECT_DIR"
ninja -C build-consumer -j2

echo ""
echo "=== [2/3] Packaging ==="
./scripts/package_consumer.sh

echo ""
echo "=== [3/3] Done ==="
echo "  binary:  package/bin/orca-slice-engine"
echo "  library: package/lib/libslic3r.so.1.0.0"
echo "  run:     ./orca-slice-engine <input.3mf>"
