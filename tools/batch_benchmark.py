#!/usr/bin/env python3
"""
Unified batch benchmark: score + slice 3MF files, compare score rank vs slice wall-clock time.

Replaces: batch_compare.py, batch_middle18.py, batch_score.py, score_middle18.py

Usage:
    python batch_benchmark.py --dir <3mf_dir>                      # All files
    python batch_benchmark.py --dir <3mf_dir> --range 18 36        # Subset by index
    python batch_benchmark.py --dir <3mf_dir> --score-only         # Score only, no slicing
    python batch_benchmark.py --dir <3mf_dir> --output results.json
"""

import argparse
import json
import os
import subprocess
import sys
import time


def compute_spearman(score_rank, time_rank, n):
    """Spearman rank correlation: 1 - 6*sum(d^2) / (n*(n^2-1))"""
    if n <= 1:
        return 0.0
    sum_d_sq = 0.0
    for fname in score_rank:
        if fname in time_rank:
            d = score_rank[fname] - time_rank[fname]
            sum_d_sq += d * d
    return 1.0 - (6.0 * sum_d_sq) / (n * (n * n - 1))


def find_tool_paths():
    """Resolve tool and engine paths relative to this script."""
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(tools_dir)
    score_tool = os.path.join(tools_dir, "3mf_score.py")
    slice_exe = os.path.join(project_dir, "build_consumer", "Release", "orca-slice-engine.exe")
    slice_dir = os.path.join(project_dir, "build_consumer", "Release")
    return score_tool, slice_exe, slice_dir


def score_file(filepath, score_tool, timeout=120):
    """Run 3mf_score.py --json, return dict with score/level/triangles/features."""
    cmd = [sys.executable, score_tool, "--json", filepath]
    try:
        r = subprocess.run(
            cmd,
            capture_output=True, text=True,
            timeout=timeout,
            env={**os.environ, "PYTHONIOENCODING": "utf-8"},
        )
        if r.returncode != 0 or not r.stdout.strip():
            return {"score": -1, "level": "error", "triangles": 0,
                    "error": f"rc={r.returncode} stderr={r.stderr[:200]}"}
        data = json.loads(r.stdout)
        features = data.get("breakdown", {}).get("geometry", {})
        layers_info = data.get("breakdown", {}).get("layers", {})
        mem_risk = data.get("memory_risk", {})
        return {
            "score": data.get("score", -1),
            "level": data.get("level", "unknown"),
            "triangles": features.get("total_triangles", 0),
            "vertices": features.get("total_vertices", 0),
            "objects": features.get("object_count", len(data.get("breakdown", {}).get("objects", []))),
            "shells": features.get("shell_count", 0),
            "estimated_layers": features.get("estimated_layers", 0),
            "layer_factor": layers_info.get("factor", 1.0),
            "bounding_box_mm": features.get("bounding_box_mm", None),
            "memory_risk": mem_risk.get("risk", "unknown"),
            "memory_index": mem_risk.get("index", 0),
        }
    except subprocess.TimeoutExpired:
        return {"score": -1, "level": "timeout", "triangles": 0, "error": f"score timeout after {timeout}s"}
    except Exception as e:
        return {"score": -1, "level": "error", "triangles": 0, "error": str(e)}


def slice_file(filepath, slice_exe, slice_dir, resources, timeout=300):
    """Run orca-slice-engine, return wall-clock time, print_time, exit_code."""
    basename = os.path.splitext(os.path.basename(filepath))[0]
    out_prefix = f"bench_{basename}"
    result = {
        "exit_code": -1,
        "wall_time_s": -1.0,
        "print_time_s": -1.0,
        "error": None,
    }
    try:
        env = os.environ.copy()
        if resources:
            env["ORCA_RESOURCES"] = resources
        env["PYTHONIOENCODING"] = "utf-8"

        t0 = time.time()
        r = subprocess.run(
            [slice_exe, "-v", "-j", "-o", out_prefix, filepath],
            capture_output=True, text=True,
            timeout=timeout,
            cwd=slice_dir,
            env=env,
        )
        result["wall_time_s"] = time.time() - t0
        result["exit_code"] = r.returncode

        # Extract print_time_seconds from stdout JSON
        for line in r.stdout.split("\n"):
            if '"print_time_seconds"' in line:
                try:
                    result["print_time_s"] = float(line.split(":")[1].strip().rstrip(","))
                except ValueError:
                    pass
                break

        if result["print_time_s"] < 0:
            combined = r.stdout + r.stderr
            for line in combined.split("\n"):
                if '"print_time_seconds"' in line:
                    try:
                        result["print_time_s"] = float(line.split(":")[1].strip().rstrip(","))
                    except ValueError:
                        pass
                    break

        if r.returncode != 0:
            result["error"] = r.stderr[:300] if r.stderr else f"exit_code={r.returncode}"

        # Cleanup output files
        for ext in [".json", ".gcode.3mf"]:
            p = os.path.join(slice_dir, out_prefix + ext)
            if os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
    except subprocess.TimeoutExpired:
        result["wall_time_s"] = float(timeout)
        result["exit_code"] = -2
        result["error"] = f"slice timeout after {timeout}s"
    except Exception as e:
        result["exit_code"] = -3
        result["error"] = str(e)

    return result


def build_ranks(results, key):
    """Assign rank 1..N sorted by key ascending. Ties get min rank."""
    sorted_items = sorted(
        [r for r in results if r.get(key, -1) > 0],
        key=lambda x: x[key]
    )
    rank_map = {}
    for i, r in enumerate(sorted_items):
        rank_map[r["filename"]] = i + 1
    # Items with invalid values get rank N+1 (worst)
    invalid = [r for r in results if r.get(key, -1) <= 0]
    invalid_rank = len(sorted_items) + 1
    if invalid:
        for i, r in enumerate(invalid):
            rank_map[r["filename"]] = invalid_rank + i
    return rank_map


def print_rank_table(results, score_rank, time_rank, title, time_key):
    """Print a rank comparison table."""
    n = len(results)
    print(f"\n{'='*90}")
    print(f"RANK COMPARISON: {title}")
    print(f"Score rank: 1 = lowest score (fastest expected). Time rank: 1 = shortest actual slice wall-clock time.")
    print(f"{'='*90}")
    print(f"{'Filename':<48} {'Score':>5} {'ScRk':>5} {'Wall_s':>8} {'Tmk':>5} {'Delta':>6}")
    print("-" * 90)

    valid = [r for r in results if r.get(time_key, -1) > 0 and r.get("score", -1) > 0]
    total_delta = 0
    for r in results:
        sr = score_rank.get(r["filename"], n + 1)
        tr = time_rank.get(r["filename"], n + 1)
        delta = abs(sr - tr)
        if r.get(time_key, -1) > 0 and r.get("score", -1) > 0:
            total_delta += delta
        fn = r["filename"][:46]
        wall = r.get(time_key, -1)
        print(f"{fn:<48} {r.get('score',-1):>5} {sr:>5} {wall:>8.1f} {tr:>5} {delta:>6}")

    if valid:
        avg_delta = total_delta / len(valid)
        spearman = compute_spearman(score_rank, time_rank, n)
        print(f"\n  Valid pairs: {len(valid)}/{n}")
        print(f"  Average rank delta: {avg_delta:.1f} positions")
        print(f"  Spearman rank correlation: {spearman:.4f}")
        if spearman < 0.1:
            print("  => Essentially zero correlation — score does not predict slice time")
        elif spearman < 0.3:
            print("  => Weak correlation")
        elif spearman < 0.5:
            print("  => Moderate correlation")
        elif spearman < 0.7:
            print("  => Strong correlation")
        else:
            print("  => Very strong correlation")


def print_top_mismatches(results, score_rank, time_rank, time_key, top_n=5):
    """Print the top N files where score rank and time rank differ most."""
    mismatches = []
    for r in results:
        sr = score_rank.get(r["filename"], len(results) + 1)
        tr = time_rank.get(r["filename"], len(results) + 1)
        if r.get(time_key, -1) > 0 and r.get("score", -1) > 0:
            mismatches.append((abs(sr - tr), sr, tr, r))
    mismatches.sort(key=lambda x: x[0], reverse=True)

    print(f"\n--- Top {top_n} largest rank mismatches ---")
    print(f"{'Filename':<48} {'Score':>5} {'ScRk':>5} {'TmRk':>5} {'Delta':>6} {'Wall_s':>8}")
    print("-" * 80)
    for delta, sr, tr, r in mismatches[:top_n]:
        fn = r["filename"][:46]
        print(f"{fn:<48} {r.get('score',-1):>5} {sr:>5} {tr:>5} {delta:>6} {r.get(time_key,-1):>8.1f}")


def main():
    parser = argparse.ArgumentParser(description="Batch benchmark: score + slice 3MF files")
    parser.add_argument("--dir", required=True, help="Directory containing .3mf files")
    parser.add_argument("--range", nargs=2, type=int, metavar=("START", "END"),
                        help="1-based index range of files to process (inclusive)")
    parser.add_argument("--output", help="Save full results to JSON file")
    parser.add_argument("--score-only", action="store_true", help="Only score, skip slicing")
    parser.add_argument("--slice-only", action="store_true", help="Only slice, skip scoring")
    parser.add_argument("--slice-exe", help="Path to orca-slice-engine.exe (auto-detect if omitted)")
    parser.add_argument("--resources", help="ORCA_RESOURCES directory path")
    parser.add_argument("--score-timeout", type=int, default=120, help="Scoring timeout in seconds")
    parser.add_argument("--slice-timeout", type=int, default=300, help="Slicing timeout in seconds")
    args = parser.parse_args()

    # Find paths
    score_tool, slice_exe_default, slice_dir_default = find_tool_paths()
    slice_exe = args.slice_exe or slice_exe_default
    slice_dir = os.path.dirname(slice_exe) if slice_exe else slice_dir_default

    if not args.score_only and not os.path.isfile(slice_exe):
        print(f"Warning: slice engine not found at {slice_exe}. Switching to --score-only.", file=sys.stderr)
        args.score_only = True

    # Collect files
    if not os.path.isdir(args.dir):
        print(f"Error: directory not found: {args.dir}", file=sys.stderr)
        sys.exit(2)

    all_files = sorted([f for f in os.listdir(args.dir) if f.endswith(".3mf")])
    if not all_files:
        print(f"Error: no .3mf files found in {args.dir}", file=sys.stderr)
        sys.exit(2)

    if args.range:
        start, end = args.range
        files = all_files[start - 1:end]
        print(f"Processing files {start}-{end} of {len(all_files)} total")
    else:
        files = all_files
        print(f"Processing all {len(files)} .3mf files")

    # Main loop
    results = []
    for i, fname in enumerate(files):
        fpath = os.path.join(args.dir, fname)
        short = fname[:55]
        print(f"\n{'='*65}")
        print(f"[{i+1}/{len(files)}] {short}")

        result = {"filename": fname}

        # --- Score ---
        if not args.slice_only:
            sd = score_file(fpath, score_tool, timeout=args.score_timeout)
            result.update(sd)
            print(f"  Score: {sd.get('score')}, Level: {sd.get('level')}, Tris: {sd.get('triangles',0)}", end="")
            if sd.get("estimated_layers", 0) > 0:
                print(f", Layers(est): {sd.get('estimated_layers')}", end="")
                print(f" x{sd.get('layer_factor', 1.0):.2f}", end="")
            if sd.get("bounding_box_mm"):
                bb = sd["bounding_box_mm"]
                print(f", BB: {bb['w']}x{bb['d']}x{bb['h']}mm", end="")
            if sd.get("memory_risk") not in (None, "unknown"):
                print(f", MemRisk: {sd['memory_risk']}", end="")
            print()
        else:
            result.update({"score": -1, "level": "n/a", "triangles": 0})

        # --- Slice ---
        if not args.score_only:
            sd = slice_file(fpath, slice_exe, slice_dir, args.resources, timeout=args.slice_timeout)
            result.update(sd)
            status = "OK" if sd["exit_code"] == 0 else f"ERR(exit={sd['exit_code']})"
            print(f"  Slice: {status}, wall={sd['wall_time_s']:.1f}s, print_time={sd['print_time_s']:.1f}s", end="")
            if sd.get("error"):
                print(f", error={sd['error'][:80]}", end="")
            print()
        else:
            result.update({"exit_code": -1, "wall_time_s": -1, "print_time_s": -1})

        results.append(result)

    # --- Report ---
    n = len(results)

    # Summary table
    print(f"\n\n{'='*95}")
    print("RESULTS SUMMARY")
    print(f"{'='*95}")
    header = f"{'#':>3} {'Filename':<48} {'Score':>5} {'Level':<10} {'Tris':>9} {'Exit':>5} {'Wall_s':>8} {'Print_s':>9}"
    print(header)
    print("-" * len(header))
    for i, r in enumerate(results):
        fn = r["filename"][:46] + (".." if len(r["filename"]) > 48 else "")
        print(f"{i+1:>3} {fn:<48} {r.get('score',-1):>5} {r.get('level','?'):<10} "
              f"{r.get('triangles',0):>9} {r.get('exit_code',-1):>5} "
              f"{r.get('wall_time_s',-1):>8.1f} {r.get('print_time_s',-1):>9.1f}")

    # Rank comparisons
    score_rank = build_ranks(results, "score")
    wall_rank = build_ranks(results, "wall_time_s")
    print_rank = build_ranks(results, "print_time_s")

    if not args.score_only:
        print_rank_table(results, score_rank, wall_rank,
                         "Score Rank vs WALL-CLOCK Slice Time", "wall_time_s")

    if not args.score_only:
        print_rank_table(results, score_rank, print_rank,
                         "Score Rank vs PRINT TIME (for reference — expected near-zero)", "print_time_s")

    if any(r.get("wall_time_s", -1) > 0 for r in results):
        print_top_mismatches(results, score_rank, wall_rank, "wall_time_s", top_n=5)

    # Save output
    if args.output:
        out_path = args.output
    else:
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"bench_results_{timestamp}.json")

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({
            "config": {
                "dir": args.dir,
                "range": list(args.range) if args.range else [1, len(files)],
                "total_files": len(all_files),
            },
            "results": results,
            "score_ranks": score_rank,
            "wall_time_ranks": wall_rank,
            "print_time_ranks": print_rank,
            "stats": {
                "total": n,
                "scored": sum(1 for r in results if r.get("score", -1) > 0),
                "sliced_ok": sum(1 for r in results if r.get("exit_code") == 0),
                "spearman_wall": compute_spearman(score_rank, wall_rank, n),
                "spearman_print": compute_spearman(score_rank, print_rank, n),
            },
        }, f, ensure_ascii=False, indent=2)
    print(f"\nResults saved to: {out_path}")


if __name__ == "__main__":
    main()
