#!/usr/bin/env python3
"""
3MF 模型切片复杂度评估工具

解析 3MF 文件中的 mesh 数据和配置，输出复杂度评分报告。

用法:
    python 3mf_score.py model.3mf                    # 文本报告
    python 3mf_score.py model.3mf --json             # JSON 输出
    python 3mf_score.py model.3mf --threshold 13     # 仅返回 exit code
"""

import argparse
import json
import os
import sys
import zipfile
from xml.etree.ElementTree import ParseError, iterparse

# 同目录模块
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model_complexity import score_model, score_geometry


# ── XML 辅助 ──────────────────────────────────────────────────

NS_3MF = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"

def _ns(tag):
    return f"{{{NS_3MF}}}{tag}"

def _tag_local(elem):
    return elem.tag.split("}")[-1] if "}" in elem.tag else elem.tag


# ── 配置解析 ──────────────────────────────────────────────────

def _parse_config(zf: zipfile.ZipFile) -> dict:
    """从 Metadata/project_settings.config 提取切片配置。"""
    import json as _json
    try:
        raw = zf.read("Metadata/project_settings.config").decode("utf-8", errors="replace")
        config = _json.loads(raw)
    except (KeyError, _json.JSONDecodeError):
        return {}

    result = {}

    # 支撑
    support_enabled = config.get("enable_support", "0")
    if support_enabled == "1":
        st = config.get("support_type", "normal(auto)")
        if "tree" in st.lower():
            result["support_type"] = "tree"
        elif "normal" in st.lower():
            result["support_type"] = "normal"
        else:
            result["support_type"] = "normal"
    else:
        result["support_type"] = "none"

    # 熨烫
    ironing = config.get("ironing_type", "no ironing")
    result["ironing_enabled"] = "no ironing" not in ironing.lower()

    # 填充密度
    infill_str = config.get("sparse_infill_density", "15%").rstrip("%")
    try:
        result["infill_density_pct"] = float(infill_str)
    except ValueError:
        result["infill_density_pct"] = 15.0

    result["mmu_painted"] = False
    return result


# ── 3MF 模型解析 (最小化，仅统计三角面片) ─────────────────────

def _parse_model_names(zf: zipfile.ZipFile) -> dict:
    """解析 Metadata/model_settings.config → {object_id: name}。"""
    try:
        raw = zf.read("Metadata/model_settings.config").decode("utf-8", errors="replace")
    except KeyError:
        return {}
    import xml.etree.ElementTree as ET
    try:
        root = ET.fromstring(raw)
    except ET.ParseError:
        return {}
    names = {}
    for obj in root.findall("object"):
        oid = obj.get("id")
        if oid is None:
            continue
        for meta in obj.findall("metadata"):
            if meta.get("key") == "name":
                names[oid] = meta.get("value", "")
    return names


def _parse_mesh_objects(file_obj) -> dict:
    """
    解析单个 .model 文件的 <resources> 段。
    返回: {object_id: {"type": "mesh"|"container", "triangles": N, "components": [...]}}
    """
    objects = {}
    current_obj = None
    in_triangles = False

    for event, elem in iterparse(file_obj, events=("start", "end")):
        t = _tag_local(elem) if event == "start" else None

        if event == "start" and t == "object":
            current_obj = elem.get("id")
            objects[current_obj] = {
                "type": "unknown",
                "triangles": 0,
                "components": [],
            }

        if event == "start" and t == "mesh" and current_obj:
            objects[current_obj]["type"] = "mesh"

        if event == "start" and t == "triangles":
            in_triangles = True

        if event == "end" and _tag_local(elem) == "triangle" and in_triangles and current_obj:
            objects[current_obj]["triangles"] += 1

        if event == "end" and _tag_local(elem) == "triangles":
            in_triangles = False

        if event == "start" and t == "components" and current_obj:
            objects[current_obj]["type"] = "container"

        if event == "end" and _tag_local(elem) == "component" and current_obj:
            comp_oid = elem.get("objectid")
            # 检测 production extension 子文件路径
            sub_path = None
            for k, v in elem.attrib.items():
                if k.endswith("}path") or k == "path":
                    sub_path = v
                    break
            objects[current_obj]["components"].append({
                "objectid": comp_oid,
                "sub_path": sub_path,
            })

        if event == "end" and _tag_local(elem) == "resources":
            elem.clear()
            return objects

        if event == "end":
            elem.clear()

    return objects


def _parse_all_objects(zf: zipfile.ZipFile) -> dict:
    """解析主模型及所有外部子模型文件。"""
    # 主模型
    try:
        f = zf.open("3D/3dmodel.model")
        main = _parse_mesh_objects(f)
        f.close()
    except KeyError:
        return {"main": {}, "external": {}}

    # 收集外部子文件路径
    sub_paths = set()
    for obj in main.values():
        for comp in obj.get("components", []):
            if comp.get("sub_path"):
                sub_paths.add(comp["sub_path"].lstrip("/"))

    # 解析子文件
    external = {}
    for sp in sub_paths:
        try:
            f = zf.open(sp)
            sub_objs = _parse_mesh_objects(f)
            f.close()
            for oid, oobj in sub_objs.items():
                external[(sp, oid)] = oobj
        except (KeyError, Exception):
            pass

    return {"main": main, "external": external}


def _parse_build_items(zf: zipfile.ZipFile) -> list:
    """解析 <build> 段。返回 [(objectid, printable_bool), ...]"""
    items = []
    try:
        f = zf.open("3D/3dmodel.model")
    except KeyError:
        return items

    in_build = False
    for event, elem in iterparse(f, events=("start", "end")):
        if event == "start" and _tag_local(elem) == "build":
            in_build = True
        if event == "end" and _tag_local(elem) == "item" and in_build:
            oid = elem.get("objectid")
            printable = elem.get("printable", "1")
            items.append((oid, printable != "0"))
        if event == "end" and _tag_local(elem) == "build":
            elem.clear()
            f.close()
            return items
        if event == "end":
            elem.clear()
    f.close()
    return items


def _find_object(oid, all_objects, sub_path=None):
    """在合并的对象字典中查找。"""
    main = all_objects["main"]
    ext = all_objects["external"]

    if oid in main:
        return main[oid]
    if sub_path:
        key = (sub_path.lstrip("/"), oid)
        if key in ext:
            return ext[key]
    for (sp, eoid), eobj in ext.items():
        if eoid == oid:
            return eobj
    return None


def _count_leaf_triangles(root_oid, all_objects, sub_path=None) -> int:
    """递归展开对象链，累加所有 leaf mesh 的三角面片数。"""
    obj = _find_object(root_oid, all_objects, sub_path)
    if obj is None:
        return 0

    if obj["type"] == "container":
        total = 0
        for comp in obj.get("components", []):
            total += _count_leaf_triangles(
                comp["objectid"], all_objects, comp.get("sub_path")
            )
        return total
    else:
        return obj["triangles"]


def _collect_mesh_info(zf: zipfile.ZipFile) -> dict:
    """从 3MF 中收集所有可打印 mesh 的三角面片统计。"""
    all_objects = _parse_all_objects(zf)
    build_items = _parse_build_items(zf)
    model_names = _parse_model_names(zf)

    # 同时计算按对象的明细
    objects = []
    total_triangles = 0

    for oid, printable in build_items:
        if not printable:
            continue

        # 先检查这个对象本身（及子对象）是否有 mesh
        tri_count = _count_leaf_triangles(oid, all_objects)

        obj_name = model_names.get(oid, f"object_{oid}")
        objects.append({
            "name": obj_name,
            "triangles": tri_count,
        })
        total_triangles += tri_count

    return {"objects": objects, "total_triangles": total_triangles}


# ── 输出格式化 ──────────────────────────────────────────────────

def _format_text_report(path: str, result: dict, mesh_info: dict) -> str:
    score = result["score"]
    level = result["level"]
    b = result["breakdown"]
    g = b["geometry"]
    c = b["config"]
    objects = mesh_info.get("objects", [])

    bar_len = 20
    filled = round(score / 20 * bar_len)
    bar = "█" * filled + "░" * (bar_len - filled)

    lines = [
        f"模型: {os.path.basename(path)}",
        "─" * 54,
        f"复杂度评分: {score}/20  [{bar}]",
        f"等级: {level} — {result['level_desc']}",
        "─" * 54,
        f"几何得分:   {g['score']:<5}  ({g['total_triangles']:,} 面片)",
    ]

    if c["multiplier"] > 1.0:
        factors_str = ", ".join(f["label"] for f in c["factors"])
        lines.append(f"配置系数:   ×{c['multiplier']}  ({factors_str})")
    else:
        lines.append(f"配置系数:   ×{c['multiplier']}  (无额外开销)")

    lines.append("─" * 54)

    if len(objects) > 1:
        lines.append("对象明细:")
        max_name = max(len(o["name"]) for o in objects) if objects else 0
        for obj in objects:
            obj_score = score_geometry(obj["triangles"])
            obj_bar_len = 14
            obj_filled = round(obj_score / 20 * obj_bar_len)
            obj_bar = "█" * obj_filled + "░" * (obj_bar_len - obj_filled)
            lines.append(
                f"  {obj['name']:<{max_name}}  {obj['triangles']:>8,} △  [{obj_bar}] {obj_score:.1f}"
            )

    lines.append("─" * 54)
    time_map = {
        "trivial": "<5 秒",
        "normal": "5-30 秒",
        "moderate": "30秒-2分钟",
        "long": "2-8 分钟",
        "extreme": "8-30+ 分钟",
    }
    lines.append(f"预估耗时: {time_map.get(level, '未知')}")
    return "\n".join(lines)


def _format_json_report(path: str, result: dict, mesh_info: dict) -> str:
    report = {
        "file": os.path.basename(path),
        "score": result["score"],
        "level": result["level"],
        "level_desc": result["level_desc"],
        "breakdown": {
            "geometry": result["breakdown"]["geometry"],
            "config": result["breakdown"]["config"],
            "objects": mesh_info.get("objects", []),
        },
        "time_estimate": {
            "trivial": "<5s",
            "normal": "5-30s",
            "moderate": "30s-2min",
            "long": "2-8min",
            "extreme": "8-30min+",
        }.get(result["level"], "unknown"),
    }
    return json.dumps(report, ensure_ascii=False, indent=2)


# ── CLI ──────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="3MF 模型切片复杂度评估工具")
    parser.add_argument("input", help="输入的 .3mf 文件路径")
    parser.add_argument("--json", action="store_true", help="以 JSON 格式输出")
    parser.add_argument(
        "-t", "--threshold", type=int, default=None,
        help="阈值模式：评分 >= 阈值时 exit code=1",
    )
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"错误: 文件不存在: {args.input}", file=sys.stderr)
        sys.exit(2)

    with zipfile.ZipFile(args.input, "r") as zf:
        config = _parse_config(zf)
        mesh_info = _collect_mesh_info(zf)

    if mesh_info["total_triangles"] == 0:
        print("错误: 3MF 中没有找到可打印的 mesh 数据", file=sys.stderr)
        sys.exit(2)

    result = score_model(mesh_info["total_triangles"], config)

    if args.threshold is not None:
        sys.exit(1 if result["score"] >= args.threshold else 0)

    if args.json:
        print(_format_json_report(args.input, result, mesh_info))
    else:
        print(_format_text_report(args.input, result, mesh_info))


if __name__ == "__main__":
    main()
