#!/usr/bin/env python3
"""Audit caller arguments against orca-slice-engine 2.2.0 tightened surfaces.

2.2.0 (vs package-02.01.06) tightened CLI/params validation in ways that turn
previously-tolerated input into hard failures (rc=1) or changed semantics.
Run this against PRODUCTION invocation logs / caller scripts BEFORE upgrading
to surface every call site that will break.

Input modes (mutually exclusive):
  --cli-log FILE   each line = one complete engine command line
  --json-log FILE  each line = one C API params_json document
  --scan DIR       recursively scan caller scripts (*.sh/*.py/*.yml/*.yaml)
                   for engine invocations (CLI tokens + params_json literals)

Exit code: 0 = clean, 1 = at least one finding.

Check rules (CR*): see BREAKING-CHANGES-2.2.0-CALLER-MIGRATION.md for the
full old/new behavior table each rule maps to.
"""

import argparse
import json
import os
import re
import shlex
import sys

CLI_FORMAT_WHITELIST = {"gcode", "gcode.3mf"}
JSON_FORMAT_WHITELIST = {"gcode", "gcode.3mf"}
# strtol-accepted forms: optional '+', digits only, no spaces/sign elsewhere.
NONNEG_INT = re.compile(r"\+?[0-9]+")

ENGINE_NAME_HINTS = ("orca-slice-engine", "slic3r_slice", "slic3r_create")


class Finding:
    def __init__(self, rule, severity, where, detail, action):
        self.rule, self.severity = rule, severity
        self.where, self.detail, self.action = where, detail, action


def audit_cli_tokens(tokens, where, out):
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in ("-p", "--plate"):
            out.append(Finding(
                "CR1", "BREAK (rc=1)", where,
                f"{t} passed — removed in 2.2.0, explicit rc=1 rejection",
                "Drop the flag; per-plate selection is C API params_json "
                '"plate_id" only. A -p request silently sliced ALL plates '
                "in 06 — check whether callers depended on that (wrong) "
                "behavior."))
        elif t in ("-t", "--timeout", "--max-size"):
            val = tokens[i + 1] if i + 1 < len(tokens) else None
            if val is not None and not NONNEG_INT.fullmatch(val):
                out.append(Finding(
                    "CR2", "BREAK (rc=1)", where,
                    f'{t} "{val}" — not a whole non-negative integer; '
                    "06 atoi() silently coerced it (e.g. to 0 = no timeout)",
                    f'Pass a whole number ≥ 0 ({"0 = no limit"}).'))
        elif t in ("-f", "--format"):
            val = tokens[i + 1] if i + 1 < len(tokens) else None
            if val is not None and val not in CLI_FORMAT_WHITELIST:
                out.append(Finding(
                    "CR3", "BREAK (rc=1)", where,
                    f'-f "{val}" — not exactly "gcode"/"gcode.3mf" '
                    "(case-sensitive); 06 silently mapped anything != gcode "
                    "to gcode.3mf",
                    'Use lowercase "gcode" or "gcode.3mf".'))
        elif t == "--skip-preset-substitution":
            out.append(Finding(
                "CR4", "SILENT CHANGE", where,
                "--skip-preset-substitution passed — flag removed, silently "
                "ignored; preset substitution + user-content stripping now "
                "ALWAYS runs",
                "Expect substituted config in output (PROCESS_SUBSTITUTED "
                "issues) and stripped user G-code/notes. No rc change."))
        i += 1


def audit_params_json(doc, where, out):
    if not isinstance(doc, dict):
        out.append(Finding(
            "CR0", "MALFORMED", where,
            "params_json is not a JSON object — engine 2.2.0 rejects with rc=1",
            "Fix the caller's JSON serialization."))
        return
    pid = doc.get("plate_id")
    if isinstance(pid, int):
        if pid < 0:
            out.append(Finding(
                "CR5", "BREAK (rc=1)", where,
                f'plate_id={pid} < 0 — rejected; 06 accepted and sliced all',
                "Omit the key or pass 0 for all plates."))
        elif pid > 0:
            out.append(Finding(
                "CR6", "SEMANTIC CHANGE", where,
                f"plate_id={pid} > 0 — now HONORED. In 06 the C API never "
                "forwarded it (every request silently sliced ALL plates)",
                "Output now contains exactly one plate; out-of-range/empty "
                "target plate now returns rc=6 (PLATE_NOT_FOUND / "
                "EMPTY_PLATE). Verify the caller reads plates[]/rc for the "
                "single-plate shape."))
    for key in ("timeout_seconds", "max_size_mb"):
        v = doc.get(key)
        if isinstance(v, int) and v < 0:
            out.append(Finding(
                "CR7", "BREAK (rc=1)", where,
                f"{key}={v} < 0 — rejected; 06 applied it as no-limit",
                "Omit the key or pass >= 0."))
    fmt = doc.get("format")
    if isinstance(fmt, str) and fmt not in JSON_FORMAT_WHITELIST:
        out.append(Finding(
            "CR8", "BREAK (rc=1)", where,
            f'format="{fmt}" — not "gcode"/"gcode.3mf" (case-sensitive); '
            '06 mapped anything != "gcode" to gcode.3mf',
            'Use lowercase "gcode" or "gcode.3mf".'))
    if "skip_preset_substitution" in doc:
        out.append(Finding(
            "CR9", "SILENT CHANGE", where,
            "skip_preset_substitution key present — silently ignored; "
            "substitution + user-content stripping now ALWAYS runs",
            "Same as CR4."))


def iter_file_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            yield f"{path}:{n}", line.rstrip("\n")


def tokenize(line):
    try:
        return shlex.split(line, posix=True)
    except ValueError:
        return line.split()


def run_cli_log(path, out):
    for where, line in iter_file_lines(path):
        if not line.strip():
            continue
        audit_cli_tokens(tokenize(line), where, out)


def run_json_log(path, out):
    for where, line in iter_file_lines(path):
        line = line.strip()
        if not line:
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError as e:
            out.append(Finding(
                "CR0", "MALFORMED", where,
                f"line is not valid JSON ({e}) — cannot audit; engine would "
                "also reject non-JSON params with rc=1",
                "Fix log extraction or caller serialization."))
            continue
        audit_params_json(doc, where, out)


JSON_KEY_RE = re.compile(
    r'"(plate_id|timeout_seconds|max_size_mb|format|skip_preset_substitution)"\s*:\s*("?[^",}]*"?)')


def run_scan(root, out):
    exts = {".sh", ".py", ".yml", ".yaml"}
    skip_dirs = {"build", "build-static", ".git", "__pycache__", "package"}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in skip_dirs]
        for fn in filenames:
            if os.path.splitext(fn)[1] not in exts:
                continue
            path = os.path.join(dirpath, fn)
            for where, line in iter_file_lines(path):
                if not any(h in line for h in ENGINE_NAME_HINTS):
                    continue
                audit_cli_tokens(tokenize(line), where, out)
                # params_json literals embedded in scripts/tests
                for m in JSON_KEY_RE.finditer(line):
                    key, raw = m.group(1), m.group(2).strip('"')
                    doc = {}
                    if key in ("plate_id", "timeout_seconds", "max_size_mb"):
                        try:
                            doc[key] = int(raw)
                        except ValueError:
                            continue
                    elif key == "format":
                        doc[key] = raw
                    else:
                        doc[key] = True
                    audit_params_json(doc, where, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--cli-log", help="file of engine command lines, one per line")
    g.add_argument("--json-log", help="file of params_json documents, one per line")
    g.add_argument("--scan", help="directory to scan for engine invocations")
    ap.add_argument("--strict", action="store_true",
                    help="also count SEMANTIC/SILENT findings toward the exit code")
    args = ap.parse_args()

    out = []
    if args.cli_log:
        run_cli_log(args.cli_log, out)
    elif args.json_log:
        run_json_log(args.json_log, out)
    else:
        run_scan(args.scan, out)

    if not out:
        print("CLEAN: no 2.2.0 tightened-surface violations found.")
        return 0

    for f in out:
        print(f"[{f.rule}] {f.severity}  {f.where}\n"
              f"    what:  {f.detail}\n"
              f"    fix:   {f.action}")
    hard = [f for f in out if f.severity.startswith("BREAK") or f.severity == "MALFORMED"]
    soft = len(out) - len(hard)
    print(f"\n{len(out)} finding(s): {len(hard)} breaking, {soft} semantic/silent.")
    print("Breaking findings WILL fail (rc=1) or change output on 2.2.0 upgrade.")
    return 1 if (hard or args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
