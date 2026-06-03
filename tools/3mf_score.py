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
    返回: {object_id: {"type": "mesh"|"container", "triangles": N, "vertices": N,
                      "bbox": {min_x, max_x, ...}, "components": [...]}}
    """
    objects = {}
    current_obj = None
    in_triangles = False
    in_vertices = False
    v_count = 0
    vx = vy = vz = None

    for event, elem in iterparse(file_obj, events=("start", "end")):
        t = _tag_local(elem) if event == "start" else None

        if event == "start" and t == "object":
            current_obj = elem.get("id")
            objects[current_obj] = {
                "type": "unknown",
                "triangles": 0,
                "vertices": 0,
                "bbox": None,
                "components": [],
            }
            v_count = 0
            vx = vy = vz = None

        if event == "start" and t == "mesh" and current_obj:
            objects[current_obj]["type"] = "mesh"

        if event == "start" and t == "vertices":
            in_vertices = True

        if event == "start" and t == "vertex" and in_vertices and current_obj:
            v_count += 1
            try:
                x = float(elem.get("x", "0"))
                y = float(elem.get("y", "0"))
                z = float(elem.get("z", "0"))
                if vx is None:
                    vx = [x, x]
                    vy = [y, y]
                    vz = [z, z]
                else:
                    if x < vx[0]: vx[0] = x
                    if x > vx[1]: vx[1] = x
                    if y < vy[0]: vy[0] = y
                    if y > vy[1]: vy[1] = y
                    if z < vz[0]: vz[0] = z
                    if z > vz[1]: vz[1] = z
            except (ValueError, TypeError):
                pass

        if event == "end" and _tag_local(elem) == "vertices":
            in_vertices = False
            if current_obj and v_count > 0:
                objects[current_obj]["vertices"] = v_count
                objects[current_obj]["bbox"] = {
                    "min_x": vx[0], "max_x": vx[1],
                    "min_y": vy[0], "max_y": vy[1],
                    "min_z": vz[0], "max_z": vz[1],
                }

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


def _collect_leaf_bbox(oid, all_objects, sub_path=None):
    """Recursively collect merged bounding box across all leaf meshes."""
    obj = _find_object(oid, all_objects, sub_path)
    if obj is None:
        return None

    if obj["type"] == "container":
        merged = None
        for comp in obj.get("components", []):
            child_bb = _collect_leaf_bbox(comp["objectid"], all_objects, comp.get("sub_path"))
            if child_bb is not None:
                if merged is None:
                    merged = dict(child_bb)
                else:
                    for k in ("min_x", "min_y", "min_z"):
                        merged[k] = min(merged[k], child_bb[k])
                    for k in ("max_x", "max_y", "max_z"):
                        merged[k] = max(merged[k], child_bb[k])
        return merged
    else:
        return obj.get("bbox")


def _collect_leaf_vertices(oid, all_objects, sub_path=None):
    """Recursively sum vertex count across all leaf meshes."""
    obj = _find_object(oid, all_objects, sub_path)
    if obj is None:
        return 0
    if obj["type"] == "container":
        total = 0
        for comp in obj.get("components", []):
            total += _collect_leaf_vertices(comp["objectid"], all_objects, comp.get("sub_path"))
        return total
    else:
        return obj.get("vertices", 0)


def _collect_mesh_info(zf: zipfile.ZipFile) -> dict:
    """从 3MF 中收集所有可打印 mesh 的几何特征。"""
    all_objects = _parse_all_objects(zf)
    build_items = _parse_build_items(zf)
    model_names = _parse_model_names(zf)

    objects = []
    total_triangles = 0
    total_vertices = 0
    merged_bbox = None
    mesh_count = 0

    for oid, printable in build_items:
        if not printable:
            continue

        tri_count = _count_leaf_triangles(oid, all_objects)
        vert_count = _collect_leaf_vertices(oid, all_objects)
        obj_bbox = _collect_leaf_bbox(oid, all_objects)

        # Count this as a mesh shell
        obj = _find_object(oid, all_objects)
        if obj and obj["type"] == "mesh":
            mesh_count += 1
        elif obj and obj["type"] == "container":
            # Count leaf mesh sub-objects as shells
            for comp in obj.get("components", []):
                child = _find_object(comp["objectid"], all_objects, comp.get("sub_path"))
                if child and child["type"] == "mesh":
                    mesh_count += 1

        # Merge bounding box
        if obj_bbox is not None:
            if merged_bbox is None:
                merged_bbox = dict(obj_bbox)
            else:
                for k in ("min_x", "min_y", "min_z"):
                    merged_bbox[k] = min(merged_bbox[k], obj_bbox[k])
                for k in ("max_x", "max_y", "max_z"):
                    merged_bbox[k] = max(merged_bbox[k], obj_bbox[k])

        obj_name = model_names.get(oid, f"object_{oid}")
        objects.append({
            "name": obj_name,
            "triangles": tri_count,
            "vertices": vert_count,
            "bbox": obj_bbox,
        })
        total_triangles += tri_count
        total_vertices += vert_count

    # Compute derived features
    bbox_dims = None
    estimated_layers = 0
    if merged_bbox is not None:
        w = merged_bbox["max_x"] - merged_bbox["min_x"]
        d = merged_bbox["max_y"] - merged_bbox["min_y"]
        h = merged_bbox["max_z"] - merged_bbox["min_z"]
        bbox_dims = {"w": round(w, 1), "d": round(d, 1), "h": round(h, 1)}
        # Estimate layer count using 0.2mm default layer height
        estimated_layers = max(1, round(h / 0.2))

    return {
        "objects": objects,
        "total_triangles": total_triangles,
        "total_vertices": total_vertices,
        "object_count": len(objects),
        "shell_count": mesh_count,
        "bounding_box_mm": bbox_dims,
        "estimated_layers": estimated_layers,
    }


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
        "normal": "10秒内",
        "moderate": "10–30秒",
        "long": "30–60秒",
        "extreme": "60秒+",
    }
    lines.append(f"预估耗时: {time_map.get(level, '未知')}")
    return "\n".join(lines)


def _format_json_report(path: str, result: dict, mesh_info: dict) -> str:
    geom = result["breakdown"]["geometry"]
    # Include new feature fields in the geometry breakdown
    geom["total_vertices"] = mesh_info.get("total_vertices", 0)
    geom["object_count"] = mesh_info.get("object_count", 0)
    geom["shell_count"] = mesh_info.get("shell_count", 0)
    geom["bounding_box_mm"] = mesh_info.get("bounding_box_mm")
    geom["estimated_layers"] = mesh_info.get("estimated_layers", 0)

    report = {
        "file": os.path.basename(path),
        "score": result["score"],
        "level": result["level"],
        "level_desc": result["level_desc"],
        "breakdown": {
            "geometry": geom,
            "config": result["breakdown"]["config"],
            "objects": mesh_info.get("objects", []),
        },
        "time_estimate": {
            "trivial": "<5s",
            "normal": "<10s",
            "moderate": "10-30s",
            "long": "30-60s",
            "extreme": "60s+",
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
