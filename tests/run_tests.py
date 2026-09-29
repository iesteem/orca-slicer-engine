#!/usr/bin/env python3
"""Test-run wrapper + JUnit failure summarizer for the engine test suites.

Why this exists: `ctest --output-on-failure | tail` loses Catch2's FAILED
blocks (they scroll past long slicing logs), and ctest only knows the target
level ("engine-integration"), not the 100+ cases inside it. Every add_test
in tests/CMakeLists.txt now also writes a JUnit XML with per-case names and
statuses, so a red run is diagnosed from the report instead of re-running
the 30-minute suite.

Usage:
    python3 tests/run_tests.py <build-dir>            # run ctest, then summarize
    python3 tests/run_tests.py --report-only <build-dir>  # summarize last reports

Exit code: the wrapped ctest's exit code (report-only: 1 if any failure is
recorded, else 0).
"""

import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Show at most this many lines of a failure block — the first lines carry the
# location and the expansion, which is what locating a failure needs; the rest
# is usually captured log noise.
MAX_LINES_PER_FAILURE = 8
MAX_CHARS_PER_LINE = 240

REPORT_GLOB = "*.junit.xml"


def summarize_report(xml_path: Path) -> tuple[list[tuple[str, str, str]], int, int]:
    """Parse one JUnit XML. Returns (failures, total_cases, failed_count).

    Each failure is (suite, case_name, text_snippet). Skipped cases are not
    failures (Catch2 emits them as <skipped/>).
    """
    failures: list[tuple[str, str, str]] = []
    total = 0
    root = ET.parse(xml_path).getroot()  # <testsuites> (or <testsuite>)
    suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
    for suite in suites:
        suite_name = suite.get("name", "?")
        for case in suite.findall("testcase"):
            total += 1
            bad = case.find("failure")
            if bad is None:
                bad = case.find("error")
            if bad is None:
                continue
            text = (bad.text or bad.get("message") or "").strip()
            snippet = "\n".join(
                line[:MAX_CHARS_PER_LINE]
                for line in text.splitlines()[:MAX_LINES_PER_FAILURE]
            )
            failures.append((suite_name, case.get("name", "?"), snippet))
    return failures, total, len(failures)


def main() -> int:
    args = sys.argv[1:]
    report_only = "--report-only" in args
    args = [a for a in args if a != "--report-only"]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    build_dir = Path(args[0])
    reports = sorted((build_dir / "tests").glob(REPORT_GLOB))

    ctest_rc = 0
    if not report_only:
        rc = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "-R", "engine",
             "--output-on-failure"],
        ).returncode
        ctest_rc = rc
        reports = sorted((build_dir / "tests").glob(REPORT_GLOB))  # re-glob after run

    if not reports:
        print(f"[run_tests] no {REPORT_GLOB} under {build_dir / 'tests'}"
              + (" — nothing to summarize" if report_only else
                 " — did the tests run / reconfigure CMake?"), file=sys.stderr)
        return ctest_rc or 2

    grand_total = grand_failed = 0
    all_failures: list[tuple[str, str, str, str]] = []
    for report in reports:
        try:
            failures, total, _ = summarize_report(report)
        except ET.ParseError as e:
            print(f"[run_tests] WARNING: cannot parse {report.name}: {e}",
                  file=sys.stderr)
            continue
        grand_total += total
        all_failures += [(report.stem, *f) for f in failures]

    grand_failed = len(all_failures)
    print()
    print("=" * 70)
    if grand_failed:
        print(f"FAILED CASES: {grand_failed}/{grand_total} "
              f"({', '.join(p.name for p in reports)})")
        for suite, case, name, snippet in all_failures:
            print("-" * 70)
            print(f"[{suite}] {name}")
            if snippet:
                print(snippet)
    else:
        print(f"ALL GREEN: {grand_total} cases passed "
              f"({', '.join(p.name for p in reports)})")
    print("=" * 70)

    return ctest_rc if not report_only else (1 if grand_failed else 0)


if __name__ == "__main__":
    sys.exit(main())
