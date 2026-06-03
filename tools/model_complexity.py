#!/usr/bin/env python3
"""
3MF Slicing Complexity Score (20-point scale)

S-curve (sigmoid) maps triangle count → 1–20, multiplied by config factors.
Zero external dependencies — Python 3 stdlib only.

Usage:
    from model_complexity import score_model, score_geometry, score_config

    result = score_model(total_triangles=234567, config={
        "support_type": "tree",
        "mmu_painted": False,
        "ironing_enabled": False,
        "infill_density_pct": 15,
    })
    # result = {"score": 14, "level": "long", "breakdown": {...}}
"""

import math

# ── S-curve parameters ─────────────────────────────────────────────
# Formula: 1 + 19 / (1 + exp(-k * (log10(triangles) - center)))
# Center at log10(500K) = 5.7 — calibrated against 55 real-world 3MF files
# so that typical models (200K–2M triangles) spread across scores 8–14.
_SIGMOID_K = 1.2
_SIGMOID_CENTER = 5.7   # log10(500,000)

SCORE_MIN = 1
SCORE_MAX = 20

# ── Level definitions (calibrated against actual slice wall-clock times) ──
LEVELS = [
    ("trivial",   1,  3,  "<5秒"),
    ("normal",    4,  7,  "10秒内"),
    ("moderate",  8, 12,  "10–30秒"),
    ("long",     13, 16,  "30–60秒，建议预留"),
    ("extreme",  17, 20,  "60秒+，建议先简化模型"),
]

# ── Config multipliers ─────────────────────────────────────────────
def score_geometry(total_triangles: int) -> float:
    """Pure geometry score via S-curve.

    Args:
        total_triangles: Total triangle count of the model.

    Returns:
        float: 1.0 ~ 20.0
    """
    if total_triangles <= 0:
        return 1.0

    logx = math.log10(total_triangles)
    exponent = -_SIGMOID_K * (logx - _SIGMOID_CENTER)
    sigmoid = 1.0 / (1.0 + math.exp(exponent))
    return SCORE_MIN + (SCORE_MAX - SCORE_MIN) * sigmoid


def score_config(config: dict) -> dict:
    """Compute config multiplier from slicing settings.

    Args:
        config: {
            "support_type": "none" | "normal" | "tree",
            "mmu_painted": bool,
            "ironing_enabled": bool,
            "infill_density_pct": float,
        }

    Returns:
        {"multiplier": 1.6, "factors": [{"label": "树状支撑", "value": 1.6}, ...]}
    """
    factors = []
    multiplier = 1.0

    support = config.get("support_type", "none")
    if support == "tree":
        multiplier *= 1.6
        factors.append({"label": "树状支撑", "value": 1.6})
    elif support == "normal":
        multiplier *= 1.3
        factors.append({"label": "普通支撑", "value": 1.3})

    if config.get("mmu_painted", False):
        multiplier *= 1.2
        factors.append({"label": "多彩绘制", "value": 1.2})

    if config.get("ironing_enabled", False):
        multiplier *= 1.15
        factors.append({"label": "熨烫", "value": 1.15})

    infill = config.get("infill_density_pct", 15)
    if infill > 25:
        multiplier *= 1.1
        factors.append({"label": "高填充密度", "value": 1.1})

    return {
        "multiplier": round(multiplier, 3),
        "factors": factors,
    }


def classify_score(score: int) -> dict:
    """Map integer score to level info."""
    for level, lo, hi, desc in LEVELS:
        if lo <= score <= hi:
            return {"level": level, "label": f"{level} ({desc})"}
    return {"level": "extreme", "label": "extreme"}


def score_model(total_triangles: int, config: dict = None) -> dict:
    """Composite score: geometry × config multiplier.

    Args:
        total_triangles: Total triangle count.
        config: Config dict, default empty (no extra multipliers).

    Returns:
        {
            "score": 14,                  # int, 1–20
            "level": "long",
            "level_desc": "long (30–60秒，建议预留)",
            "breakdown": {
                "geometry": {"total_triangles": 234567, "score": 10.5},
                "config": {"multiplier": 1.6, "factors": [...]},
                "objects": None,
            },
        }
    """
    if config is None:
        config = {}

    geom_score = score_geometry(total_triangles)
    cfg = score_config(config)

    raw = geom_score * cfg["multiplier"]
    final_score = min(SCORE_MAX, max(SCORE_MIN, round(raw)))

    level_info = classify_score(final_score)

    return {
        "score": final_score,
        "level": level_info["level"],
        "level_desc": level_info["label"],
        "breakdown": {
            "geometry": {
                "total_triangles": total_triangles,
                "score": round(geom_score, 1),
            },
            "config": {
                "multiplier": cfg["multiplier"],
                "factors": cfg["factors"],
            },
            "objects": None,
        },
    }


# ── Reference lookup table ────────────────────────────────────────

def lookup_table():
    """Print triangle count → score reference table."""
    print(f"S-curve: center=log10(500K)={_SIGMOID_CENTER}, k={_SIGMOID_K}")
    print()
    print(f"{'Triangles':>12}  {'log10':>6}  {'Score':>6}  Level")
    print("-" * 44)
    for n in [100, 1000, 5000, 10000, 50000,
              100000, 200000, 300000, 500000,
              800000, 1000000, 2000000, 5000000, 10000000, 20000000]:
        s = score_geometry(n)
        info = classify_score(round(s))
        print(f"{n:>12,}  {math.log10(n):>6.2f}  {s:>5.1f}  {info['level']}")


if __name__ == "__main__":
    lookup_table()
