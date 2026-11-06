#!/usr/bin/env python3
"""Test-run wrapper + JUnit failure summarizer for the engine test suites.

Why this exists: `ctest --output-on-failure | tail` loses Catch2's FAILED
blocks (they scroll past long slicing logs), and ctest only knows the target
level ("engine-integration"), not the 100+ cases inside it. Every add_test
in tests/CMakeLists.txt now also writes a JUnit XML with per-case names and
statuses, so a red run is diagnosed from the report instead of re-running
the 30-minute suite.

Guards (B2+B5, 2026-11-06 — see the junit-report truncation trap: a
SIGTERM-killed run leaves a LEGAL XML containing only the cases that ran;
that shape used to summarize as green):
  1. parse       — unparseable report is never green (pre-existing);
  2. name subset — every case the binary lists (--list-tests) must appear
                   in its junit report; missing names = truncated run;
  3. freshness   — a report older than its own binary is a stale leftover
                   from a previous build, not this run's result;
  4. ctest rc    — a non-zero ctest rc suppresses ALL GREEN regardless of
                   what parsed (a killed binary can leave a clean-looking
                   partial report behind).

Usage:
    python3 tests/run_tests.py <build-dir>            # run ctest, then summarize
    python3 tests/run_tests.py --report-only <build-dir>  # summarize last reports

Exit code: the wrapped ctest's exit code if non-zero; else 1 when any
failure is recorded or any guard trips (report-only: 1 under the same
conditions, else 0).
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

# Cap the per-report listing of guard-violating case names — three to five
# names identify the truncation point; the rest is noise.
MAX_MISSING_NAMES_SHOWN = 5

# mtime tolerance (s) for the freshness guard — filesystem timestamp
# coarseness only, a real rerun writes the report minutes after the build.
FRESHNESS_SLACK_S = 1

REPORT_GLOB = "*.junit.xml"


def summarize_report(xml_path: Path) -> tuple[list[tuple[str, str, str]], int, set[str]]:
    """Parse one JUnit XML. Returns (failures, total_cases, case_names).

    Each failure is (suite, case_name, text_snippet); case_names is the set
    of ALL testcase names seen (feeding the name-subset guard). Skipped
    cases are not failures (Catch2 emits them as <skipped/>).
    """
    failures: list[tuple[str, str, str]] = []
    names: set[str] = set()
    total = 0
    root = ET.parse(xml_path).getroot()  # <testsuites> (or <testsuite>)
    suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
    for suite in suites:
        suite_name = suite.get("name", "?")
        for case in suite.findall("testcase"):
            total += 1
            names.add(case.get("name", "?"))
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
    return failures, total, names


def listed_case_names(binary: Path) -> set[str] | None:
    """Case names the test binary declares (`--list-tests -r xml`).

    None = binary missing or its listing failed (guard silently skipped —
    report-only against copied reports must keep working); an empty set
    from a runnable binary is returned as-is and will trip the guard.
    """
    if not binary.is_file():
        return None
    try:
        p = subprocess.run([str(binary), "--list-tests", "-r", "xml"],
                           capture_output=True, text=True, timeout=120)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if p.returncode != 0:
        return None
    # The integration binary links SliceEngine: static initializers emit
    # boost [trace] lines on stdout BEFORE the reporter XML. Anchor on the
    # root element, not ET.fromstring(p.stdout).
    xml_start = p.stdout.find("<MatchingTests")
    if xml_start < 0:
        return None
    try:
        root = ET.fromstring(p.stdout[xml_start:])
        return {tc.findtext("Name") or "" for tc in root.findall("TestCase")}
    except ET.ParseError:
        return None


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
    invalid_reports: list[str] = []
    stale_reports: list[str] = []
    truncated_reports: list[str] = []
    for report in reports:
        try:
            failures, total, names = summarize_report(report)
        except ET.ParseError as e:
            # An unparseable report is NOT "0 failures": SIGTERM mid-run
            # leaves exactly this shape (see the junit-report truncation
            # trap). Count it as results-incomplete, never green
            # (external PR #10 review, 2026-10-16).
            print(f"[run_tests] WARNING: cannot parse {report.name}: {e}",
                  file=sys.stderr)
            invalid_reports.append(report.name)
            continue
        grand_total += total
        all_failures += [(report.stem, *f) for f in failures]

        # ---- guards (skipped when the sibling binary is absent, e.g.
        # report-only against copied reports). NOTE: report.stem would be
        # "engine-tests.junit" (stem strips only the LAST suffix) — slice
        # the glob suffix off instead. ----
        binary = report.parent / report.name[: -len(".junit.xml")]
        listed = listed_case_names(binary)
        if listed is None:
            print(f"[run_tests] note: no usable binary for {report.name} — "
                  f"subset/freshness guard skipped", file=sys.stderr)
            continue
        if report.stat().st_mtime < binary.stat().st_mtime - FRESHNESS_SLACK_S:
            stale_reports.append(report.name)
        # Catch2 junit expands SECTIONs: a testcase element's name is
        # "case/section-path", and a case whose every leaf ran may still
        # never appear under its bare name. A listed case is covered by an
        # exact match OR any of its section leaves.
        missing = {l for l in listed
                   if l not in names
                   and not any(n.startswith(l + "/") for n in names)}
        if missing:
            truncated_reports.append(report.name)
            shown = ", ".join(sorted(missing)[:MAX_MISSING_NAMES_SHOWN])
            print(f"[run_tests] WARNING: {report.name} is missing "
                  f"{len(missing)} case(s) the binary lists "
                  f"(e.g. {shown}) — truncated report", file=sys.stderr)

    grand_failed = len(all_failures)
    guards_tripped = bool(invalid_reports or stale_reports or truncated_reports)
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
    elif invalid_reports:
        print(f"RESULTS INCOMPLETE: {len(invalid_reports)} report(s) "
              f"unparseable, {grand_total} cases parsed OK — "
              f"do NOT read this run as green")
    elif stale_reports or truncated_reports:
        print(f"RESULTS INCOMPLETE: stale {stale_reports or '—'}, "
              f"truncated {truncated_reports or '—'} "
              f"({grand_total} cases parsed OK) — "
              f"do NOT read this run as green")
    elif ctest_rc:
        # Guard 4: the reports may look clean while ctest itself failed —
        # e.g. a binary killed by a watchdog leaves last run's report or a
        # parseable partial behind. A non-zero ctest rc forbids ALL GREEN.
        print(f"CTEST rc={ctest_rc} with clean-looking reports — "
              f"do NOT read this run as green")
    else:
        print(f"ALL GREEN: {grand_total} cases passed "
              f"({', '.join(p.name for p in reports)})")
    for name in invalid_reports:
        print(f"INVALID REPORT: {name}")
    print("=" * 70)

    if report_only:
        return 1 if (grand_failed or guards_tripped) else 0
    # Normal mode keeps ctest's verdict first; a green ctest alongside a
    # tripped guard is itself anomalous — surface it instead of 0.
    return ctest_rc if ctest_rc else (1 if guards_tripped else 0)


if __name__ == "__main__":
    sys.exit(main())
