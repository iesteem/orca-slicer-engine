#!/usr/bin/env python3
"""
3MF 模型切片复杂度评分库

S 型曲线几何评分 + 配置系数加权，20 分制。
零外部依赖，仅使用 Python 3 标准库。

用法:
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

# ── S 型曲线参数 ────────────────────────────────────────────────
# 公式：1 + 19 / (1 + exp(-k * (log10(triangles) - center)))
_SIGMOID_K = 1.2       # 斜率：越大中间越陡
_SIGMOID_CENTER = 4.7  # 中心点：log10(50K)=4.7 处斜率最大

SCORE_MIN = 1
SCORE_MAX = 20

# ── 等级定义 ──────────────────────────────────────────────────
LEVELS = [
    ("trivial",  1,  3,  "秒级，即时返回"),
    ("normal",   4,  7,  "10秒内，无感知"),
    ("moderate", 8,  12, "1分钟左右"),
    ("long",     13, 16, "2-8分钟，建议预留"),
    ("extreme",  17, 20, "10分钟+，建议先简化模型"),
]

# ── 配置系数 ──────────────────────────────────────────────────
# 键: 配置条件 → (系数, 标签)
_CONFIG_MULTIPLIERS = [
    # (检查项, 匹配值, 系数, 标签)
    # 检查逻辑: config.get(key) == value
    ("support_type", "tree",   1.6, "树状支撑"),
    ("support_type", "normal", 1.3, "普通支撑"),
    ("mmu_painted",  True,     1.2, "多彩绘制"),
    ("ironing_enabled", True,  1.15, "熨烫"),
    ("infill_density_pct", ..., 1.1, "高填充密度"),  # ... 表示比较 > 25
]


def score_geometry(total_triangles: int) -> float:
    """
    纯几何评分，S 型曲线映射。

    Args:
        total_triangles: 模型三角面片总数

    Returns:
        float: 1.0 ~ 20.0 的评分
    """
    if total_triangles <= 0:
        return 1.0

    logx = math.log10(total_triangles)
    exponent = -_SIGMOID_K * (logx - _SIGMOID_CENTER)
    sigmoid = 1.0 / (1.0 + math.exp(exponent))
    return SCORE_MIN + (SCORE_MAX - SCORE_MIN) * sigmoid


def score_config(config: dict) -> dict:
    """
    计算配置系数。

    Args:
        config: {
            "support_type": "none" | "normal" | "tree",   # 默认 "none"
            "mmu_painted": bool,                           # 默认 False
            "ironing_enabled": bool,                       # 默认 False
            "infill_density_pct": float,                   # 默认 15
        }

    Returns:
        {"multiplier": 1.6, "factors": [{"label": "树状支撑", "value": 1.6}, ...]}
    """
    factors = []
    multiplier = 1.0

    # 支撑类型
    support = config.get("support_type", "none")
    if support == "tree":
        multiplier *= 1.6
        factors.append({"label": "树状支撑", "value": 1.6})
    elif support == "normal":
        multiplier *= 1.3
        factors.append({"label": "普通支撑", "value": 1.3})

    # 多彩绘制
    if config.get("mmu_painted", False):
        multiplier *= 1.2
        factors.append({"label": "多彩绘制", "value": 1.2})

    # 熨烫
    if config.get("ironing_enabled", False):
        multiplier *= 1.15
        factors.append({"label": "熨烫", "value": 1.15})

    # 高填充密度
    infill = config.get("infill_density_pct", 15)
    if infill > 25:
        multiplier *= 1.1
        factors.append({"label": "高填充密度", "value": 1.1})

    return {
        "multiplier": round(multiplier, 3),
        "factors": factors,
    }


def classify_score(score: int) -> dict:
    """根据得分返回等级信息。"""
    for level, lo, hi, desc in LEVELS:
        if lo <= score <= hi:
            return {"level": level, "label": f"{level} ({desc})"}
    return {"level": "extreme", "label": "extreme"}


def score_model(total_triangles: int, config: dict = None) -> dict:
    """
    综合评分（几何 × 配置系数）。

    Args:
        total_triangles: 总三角面片数
        config: 配置字典，默认空（无额外系数）

    Returns:
        {
            "score": 14,                  # int, 1-20
            "level": "long",              # trivial|normal|moderate|long|extreme
            "level_desc": "2-8分钟",
            "breakdown": {
                "geometry": {
                    "total_triangles": 234567,
                    "score": 10.5,
                },
                "config": {
                    "multiplier": 1.6,
                    "factors": [...],
                },
                "objects": None,          # 可选，子对象明细
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


# ── 查找表：快速参考 ──────────────────────────────────────────

def lookup_table():
    """打印常用面片数 → 几何分对照表。"""
    print(f"{'三角面片':>12}  {'log10':>6}  {'几何分':>7}  等级")
    print("-" * 44)
    for n in [12, 100, 500, 1000, 5000, 10000, 50000,
              100000, 200000, 500000, 1000000, 2000000]:
        s = score_geometry(n)
        info = classify_score(round(s))
        print(f"{n:>12,}  {math.log10(n):>6.2f}  {s:>6.1f}  {info['level']}")


if __name__ == "__main__":
    lookup_table()
