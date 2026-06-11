#!/usr/bin/env python3
"""
3MF Slicing Complexity Score (20-point scale)

S-curve (sigmoid) maps triangle count -> 1-20, multiplied by config factors.
Zero external dependencies -- Python 3 stdlib only.

---- Caller Decision Logic ----

The scoring system provides two orthogonal signals:

  score (1-20)       -> slice wall-clock time estimate
  memory_risk (str)  -> G-code export OOM probability

They are correlated but not equivalent. A model can be high-score but
memory-safe (e.g. planar model with many triangles), or low-score but
memory-risky (tall model with few triangles).

Calibrated against 22-model k3s slicing run on 16 GiB pod (2026-06-11):

                        memory_risk
                  low      medium      high       critical
  score          (safe)   (safe)     (>=32GiB)   (>=32GiB, may OOM)
  ----------    -------  --------   ---------   ----------------
  trivial (1-3)    2         -           -            -
  normal  (4-7)    2         -           -            -
  moderate(8-12)   7         4           1            -
  long   (13-16)    7         1           -            -
  extreme(17-20)    7         4           4            4

  Cell value = number of models in that bucket.
  Total OOMs: 3, all in (extreme, critical).

Recommended caller rules:

  1. Pod sizing (memory_risk):
       low / medium  -> standard 16 GiB pod
       high           -> 32 GiB pod recommended
       critical       -> 32 GiB pod required; if still OOM, mark unslicable

  2. Timeout (score):
       trivial/normal ->  60s timeout
       moderate       -> 180s timeout
       long           -> 300s timeout
       extreme        -> 600s timeout

  3. Queue priority (score):
       Trivial/normal models can be fast-tracked; extreme models should
       be scheduled on idle nodes to avoid blocking the queue.

Usage:
    from model_complexity import score_model, score_geometry, score_config

    result = score_model(total_triangles=234567, config={
        "support_type": "tree",
        "mmu_painted": False,
        "ironing_enabled": False,
        "infill_density_pct": 15,
    })
    # result = {"score": 14, "level": "long", "memory_risk": {...}, "breakdown": {...}}
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
# Score predicts slice wall-clock time only. Use memory_risk for OOM prediction.
# 22-model k3s validation (2026-06-11): 19/22 score>=17 models succeeded.
# Score alone is NOT an OOM predictor — see assess_memory_risk() instead.
LEVELS = [
    ("trivial",   1,  3,  "<5s"),
    ("normal",    4,  7,  "<10s"),
    ("moderate",  8, 12,  "10-30s"),
    ("long",     13, 16,  "30-60s"),
    ("extreme",  17, 20,  "60s+, check memory_risk for OOM"),
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


def score_layer_factor(estimated_layers: int) -> float:
    """Sigmoid multiplier for layer count (G-code export memory driver).

    The engine holds all plate G-code in memory until write completes.
    Memory during export scales with total layers × per-layer complexity.
    Tall models with many layers are at disproportionate risk of OOM,
    even when triangle count is moderate.

    Returns:
        1.0 (<=100 layers) to ~1.6 (5000+ layers)
    """
    if estimated_layers <= 100:
        return 1.0
    logx = math.log10(estimated_layers)
    # Center at log10(800) ≈ 160 mm at 0.2 mm layer height
    exponent = -2.0 * (logx - 2.9)
    sigmoid = 1.0 / (1.0 + math.exp(exponent))
    return 1.0 + 0.6 * sigmoid


def assess_memory_risk(total_triangles: int, estimated_layers: int,
                       object_count: int = 1) -> dict:
    """Assess G-code export OOM risk.

    The engine holds all plate G-code in memory until write completes.
    Total G-code volume ∝ layers × per-layer complexity.
    We proxy per-layer complexity with sqrt(triangles), giving:
        memory_index = layers × sqrt(triangles).

    object_count is accepted for API compatibility but NOT factored into
    the index — multi-object models spread the same total triangles across
    more sub-objects, which adds G-code boilerplate but does not multiply
    per-layer memory pressure.  k3s validation confirmed that object_factor
    falsely inflated indices (e.g. 机甲暴龙 scored higher than 醒狮眼镜仔
    but succeeded while 醒狮眼镜仔 OOMed).

    Thresholds calibrated against 22-model k3s slicing run (2026-06-11):
      - 醒狮眼镜仔(6):  523 × sqrt(9.2M)  = 1.59M → OOM
      - 醒狮眼镜仔:     523 × sqrt(18.5M) = 2.25M → OOM
      - 机甲暴龙:        727 × sqrt(1.3M)  = 822K → success
      - 中式楼阁:        693 × sqrt(7.9M)  = 1.95M → success (134s, heavy)

    Returns:
        {"risk": "low"|"medium"|"high"|"critical",
         "index": float, "label": str}
    """
    if estimated_layers <= 0 or total_triangles <= 0:
        return {"risk": "unknown", "index": 0,
                "label": "unknown (insufficient layer/triangle data)"}

    memory_index = estimated_layers * math.sqrt(total_triangles)

    if memory_index < 300_000:
        risk = "low"
    elif memory_index < 750_000:
        risk = "medium"
    elif memory_index < 1_500_000:
        risk = "high"
    else:
        risk = "critical"

    labels = {
        "low":      "low — G-code export memory is manageable",
        "medium":   "medium — monitor memory during export",
        "high":     "high — significant memory pressure, recommend >=32 GiB pod",
        "critical": "critical — high probability of OOM during G-code export",
    }
    return {"risk": risk, "index": round(memory_index), "label": labels[risk]}


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


def score_model(total_triangles: int, config: dict = None,
                estimated_layers: int = 0, object_count: int = 1) -> dict:
    """Composite score: geometry × config multiplier × layer factor.

    Score (1-20) predicts slice wall-clock time.
    memory_risk predicts G-code export OOM probability.
    These are independent dimensions — a high score does NOT imply OOM risk,
    and a low score does NOT guarantee memory safety.

    Decision logic (calibrated against 22-model k3s run, 2026-06-11):
        if mem_risk["risk"] == "critical":
            # High OOM probability — reject or require 32+ GiB pod
        elif mem_risk["risk"] == "high":
            # Significant memory pressure — recommend >=32 GiB pod
        else:
            # Safe for standard 16 GiB pod

    Args:
        total_triangles: Total triangle count across all printable objects.
        config: Config dict, default empty (no extra multipliers).
        estimated_layers: Estimated layer count (Z-height / 0.2 mm).
                          Used for G-code export memory risk. 0 = unknown.
        object_count: Accepted for API compatibility, no longer affects index.

    Returns:
        {
            "score": 14,                  # int, 1–20
            "level": "long",
            "level_desc": "long (30-60s, monitor memory)",
            "memory_risk": {"risk": "medium", "index": 750000, "label": "..."},
            "breakdown": {
                "geometry": {"total_triangles": 234567, "score": 10.5},
                "layers": {"estimated_layers": 800, "factor": 1.30},
                "config": {"multiplier": 1.6, "factors": [...]},
                "objects": None,
            },
        }
    """
    if config is None:
        config = {}

    geom_score = score_geometry(total_triangles)
    cfg = score_config(config)
    layer_factor = score_layer_factor(estimated_layers)

    raw = geom_score * cfg["multiplier"] * layer_factor
    final_score = min(SCORE_MAX, max(SCORE_MIN, round(raw)))

    level_info = classify_score(final_score)
    mem_risk = assess_memory_risk(total_triangles, estimated_layers, object_count)

    return {
        "score": final_score,
        "level": level_info["level"],
        "level_desc": level_info["label"],
        "memory_risk": mem_risk,
        "breakdown": {
            "geometry": {
                "total_triangles": total_triangles,
                "score": round(geom_score, 1),
            },
            "layers": {
                "estimated_layers": estimated_layers,
                "factor": round(layer_factor, 2),
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
