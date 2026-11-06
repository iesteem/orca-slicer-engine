#!/bin/bash
# =============================================================================
# nightly_tests.sh — nightly full-suite run + durable JUnit archive
#
# Why this exists (TECH-DESIGN-TEST-SYSTEM-2026-10-12 §5.1 M0): test value =
# coverage × execution frequency. Until CI lands, this script is the
# "most recent green exists here" proof — and the before/after snapshot pair
# the base-bump checklist (docs/process/base-bump-checklist.md §0/§7) runs on.
#
# The JUnit reports in build-static/tests/ are overwritten in place by every
# run (and a SIGTERM-killed run leaves a legal-looking partial XML — see the
# junit truncation trap), so the dated copy under test-archives/ is the only
# durable evidence.
#
# Usage:
#   scripts/nightly_tests.sh          # build (incremental, -j 2) + full suite
#
# Output:
#   test-archives/<YYYY-MM-DD>/
#   ├── nightly.log       # build + ctest console output
#   ├── engine-tests.junit.xml
#   ├── engine-integration.junit.xml
#   └── rc                # wrapped ctest exit code (0 = green)
#
# Stall sentinels (B1, 2026-11-06 — archives were once manually wiped and the
# break went unnoticed for weeks; cron deaths write nothing):
#   test-archives/_STATUS   # heartbeat, rewritten on every COMPLETED cycle
#   test-archives/STALLED   # appended when a run starts ≥26h after the last
#                           # completed cycle; removed only by a completed one
#
# Cron entry (registered 2026-10-12, remove with `crontab -e`):
#   17 2 * * * /home/joyx/Desktop/code/orca-slice-engine/scripts/nightly_tests.sh \
#     >> /home/joyx/Desktop/code/orca-slice-engine/test-archives/cron.log 2>&1
# =============================================================================

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-static"
ARCHIVE_ROOT="$PROJECT_DIR/test-archives"
KEEP_DAYS=14

# cron's PATH is minimal (/usr/bin:/bin); cmake/gmake/python3 all live in
# /usr/bin on this host (verified 2026-10-12), but pin it anyway so a user
# PATH change can't silently redirect the tools.
export PATH="/usr/bin:/bin"

# ---- re-entry guard: never stack a second run on a hung one ----
exec 9>"/tmp/orca-nightly-tests-$UID.lock"
if ! flock -n 9; then
    echo "[nightly] previous run still active, exiting"
    exit 0
fi

# ---- stall detection (runs BEFORE the disk guard so the currently most
# common death — the 3GiB abort — still leaves a trace): _STATUS is only
# written by a COMPLETED archive cycle. A run dying later tonight (disk,
# build, hang) updates nothing, and the next run that manages to start
# finds a stale heartbeat and appends the evidence to STALLED. 26h = one
# nightly cadence (24h) + the run's own duration and start-time drift. ----
STATUS_FILE="$ARCHIVE_ROOT/_STATUS"
STALLED_FILE="$ARCHIVE_ROOT/STALLED"
if [ -f "$STATUS_FILE" ]; then
    last="$(stat -c %Y "$STATUS_FILE")"
    age_h=$(( ( $(date +%s) - last ) / 3600 ))
    if [ "$age_h" -ge 26 ]; then
        printf '%s no completed nightly for %dh (last completed %s)\n' \
            "$(date -Is)" "$age_h" "$(date -Is -d "@$last")" >> "$STALLED_FILE"
        echo "[nightly] STALL detected: no completed cycle for ${age_h}h — recorded in $STALLED_FILE"
    fi
fi

# ---- disk pre-check: a full disk cascades into GCODE_EXPORT_ERROR and
# orphaned gcode.tmp files mid-suite (disk-space slicing ops precedent);
# the suite itself needs headroom for per-fixture gcode.3mf outputs.
# 3GiB floor: the suite's measured need is <1GiB (2026-10-12 full run left
# 3.1GiB free), and this host's steady state is ~90% full — a 5GiB floor
# would abort most nights and make the archive unreliable. ----
avail_kb="$(df --output=avail "$PROJECT_DIR" | tail -n1)"
if [ "$avail_kb" -lt 3145728 ]; then
    die "less than 3GiB free (${avail_kb}kB) — aborting before the suite fills the disk"
fi

day="$(date +%F)"
dest="$ARCHIVE_ROOT/$day"
mkdir -p "$dest"

# ---- build + run, all console output into the dated log ----
rc=0
{
    echo "[nightly] === $day incremental build (-j 2 per P-14) ==="
    cmake --build "$BUILD_DIR" --target engine-tests engine-integration -j 2 \
        || die "build failed"
    echo "[nightly] === full suite via run_tests.py ==="
    # Red runs must still be archived — capture the rc instead of failing out.
    set +e
    python3 "$PROJECT_DIR/tests/run_tests.py" "$BUILD_DIR"
    rc=$?
    set -e
    echo "[nightly] wrapped ctest rc=$rc"
} > "$dest/nightly.log" 2>&1

# ---- archive the reports + verdict ----
cp "$BUILD_DIR"/tests/*.junit.xml "$dest/"
echo "$rc" > "$dest/rc"

# Case-count guard from the junit truncation trap: a report with far fewer
# <testcase> entries than the current baseline (212 total) is a partial run,
# not a green run. Record the counts next to the verdict so a human reading
# the archive can spot it without re-parsing.
for xml in "$dest"/*.junit.xml; do
    echo "$(basename "$xml"): $(grep -c '<testcase' "$xml") cases" >> "$dest/counts.txt"
done

# ---- heartbeat: this run COMPLETED a full archive cycle (green or red —
# both are healthy observability). Only here may a previous stall clear. ----
printf '%s rc=%s %s\n' "$(date -Is)" "$rc" \
    "$(tr '\n' ' ' < "$dest/counts.txt" 2>/dev/null)" > "$STATUS_FILE"
rm -f "$STALLED_FILE"

# ---- rotation: keep only the last KEEP_DAYS days ----
find "$ARCHIVE_ROOT" -mindepth 1 -maxdepth 1 -type d -mtime +"$KEEP_DAYS" -exec rm -rf {} +

if [ "$rc" -ne 0 ]; then
    echo "[nightly] RED run archived at $dest (see nightly.log + junit)"
else
    echo "[nightly] green run archived at $dest"
fi

exit "$rc"
