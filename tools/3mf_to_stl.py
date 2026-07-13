#!/usr/bin/env python3
"""
3MF → STL 导出工具

从 3MF 文件中提取模型数据，导出为二进制 STL 文件，打包为 ZIP。
零外部依赖，仅使用 Python 3 标准库。

支持：
- 标准 3MF（mesh 内联在 3dmodel.model）
- 3MF Production Extension（mesh 存储在 3D/Objects/*.model 子文件中）

用法:
    python 3mf_to_stl.py input.3mf [-o output_dir]
"""

import argparse
import io
import os
import re
import struct
import sys
import tempfile
import zipfile
from xml.etree.ElementTree import ParseError, iterparse

# 复杂度评分（可选，仅在 --score 时加载）
try:
    from model_complexity import score_model
    _HAS_SCORER = True
except ImportError:
    _HAS_SCORER = False


# ── 3MF namespace ──────────────────────────────────────────────
NS_3MF = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"


def ns(tag):
    """返回带命名空间的完整标签名"""
    return f"{{{NS_3MF}}}{tag}"


# ── XML 辅助 ───────────────────────────────────────────────────

def tag_local(elem):
    """获取去掉命名空间的本地标签名"""
    return elem.tag.split("}")[-1] if "}" in elem.tag else elem.tag


def strip_undeclared_ns(xml_bytes: bytes) -> bytes:
    """
    修复未声明的 namespace prefix 问题。

    某些 3MF（如 BambuStudio 生成）使用 p: 前缀但未声明 xmlns:p，
    导致 ET.iterparse 抛出 "unbound prefix" 错误。
    此函数在 XML 头部添加缺失的命名空间声明。
    """
    text = xml_bytes.decode("utf-8", errors="replace")

    # 查找所有使用的 namespace 前缀
    used_prefixes = set()
    for m in re.finditer(r'(\w+):\w+\s*=', text[:100000]):  # 只扫描前 100KB
        used_prefixes.add(m.group(1))

    # 查找已声明的 xmlns 前缀
    declared = set()
    for m in re.finditer(r'xmlns:(\w+)="', text[:10000]):
        declared.add(m.group(1))

    # 移除标准前缀
    used_prefixes.discard("xmlns")
    used_prefixes.discard("xml")

    # 找出未声明的前缀
    missing = used_prefixes - declared

    if missing:
        # 在 <model> 标签中注入缺失的命名空间
        # 生产扩展命名空间
        ns_map = {
            "p": 'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06"',
            "slic3rpe": 'xmlns:slic3rpe="http://schemas.slic3r.org/3mf/2017/06"',
        }
        injections = " ".join(
            ns_map[p] for p in missing if p in ns_map
        )
        if injections:
            # 在 <model 的第一个 > 前插入
            text = re.sub(
                r'(<model\s[^>]*?)(>)',
                rf'\1 {injections}\2',
                text,
                count=1,
            )
    return text.encode("utf-8")


def safe_iterparse(file_obj):
    """
    安全的 iterparse，自动修复 namespace prefix 问题后重试。
    """
    try:
        return iterparse(file_obj, events=("start", "end"))
    except ParseError as e:
        if "unbound prefix" in str(e):
            # 读取文件内容并修复
            file_obj.seek(0)
            raw = file_obj.read()
            fixed = strip_undeclared_ns(raw)
            return iterparse(io.BytesIO(fixed), events=("start", "end"))
        raise


# ── XML 解析 ───────────────────────────────────────────────────

def parse_model_settings(zf: zipfile.ZipFile) -> dict:
    """
    解析 Metadata/model_settings.config → {object_id_str: name}
    如果文件不存在，返回空 dict。
    """
    try:
        content = zf.read("Metadata/model_settings.config").decode("utf-8")
    except KeyError:
        return {}

    names = {}
    import xml.etree.ElementTree as ET
    try:
        root = ET.fromstring(content)
    except ET.ParseError as e:
        if "unbound prefix" in str(e):
            # Fix undeclared namespace prefixes before retry
            import io
            fixed_bytes = strip_undeclared_ns(content.encode("utf-8"))
            # If strip_undeclared_ns didn't help (e.g. different root element),
            # fall back to a more aggressive regex-based prefix removal
            try:
                root = ET.fromstring(fixed_bytes.decode("utf-8"))
            except ET.ParseError:
                # Aggressive fix: strip all namespace prefixes from tags and attributes
                cleaned = re.sub(r'(</?)\w+:', r'\1', content)
                cleaned = re.sub(r'\s+xmlns:\w+="[^"]*"', '', cleaned)
                root = ET.fromstring(cleaned)
        else:
            raise
    for obj in root.findall("object"):
        oid = obj.get("id")
        for meta in obj.findall("metadata"):
            if meta.get("key") == "name":
                names[oid] = meta.get("value", "")
    return names


def parse_title(zf: zipfile.ZipFile) -> str:
    """
    从 3dmodel.model 中提取 <metadata name="Title"> 的值。
    """
    try:
        f = zf.open("3D/3dmodel.model")
        for event, elem in safe_iterparse(f):
            if event != "start":
                elem.clear()
                continue
            if tag_local(elem) == "metadata" and elem.get("name") == "Title":
                title = (elem.text or "").strip()
                elem.clear()
                f.close()
                return title
            if tag_local(elem) == "resources":
                elem.clear()
                break
            elem.clear()
        f.close()
    except KeyError:
        pass
    return ""


def _parse_one_model_resources(file_obj) -> dict:
    """
    解析单个 .model 文件的 <resources> 段，返回内部对象字典。

    同时收集 component 中引用的外部子文件路径。
    """
    objects = {}
    current_obj = None
    current_tag = None
    vertices_buf = []
    triangles_buf = []

    for event, elem in safe_iterparse(file_obj):
        if event == "start":
            t = tag_local(elem)
        else:
            t = None  # end 事件不需要标签名

        # ── 进入 <object> ──
        if event == "start" and t == "object":
            current_obj = elem.get("id")
            objects[current_obj] = {
                "type": "unknown",
                "vertices": [],
                "triangles": [],
                "components": [],
            }

        # ── <mesh> 出现 → 标记为 leaf mesh ──
        if event == "start" and t == "mesh" and current_obj:
            objects[current_obj]["type"] = "mesh"

        # ── 解析 vertices ──
        if event == "start" and t == "vertices":
            current_tag = "vertices"
            vertices_buf = []

        if event == "end" and tag_local(elem) == "vertex" and current_tag == "vertices":
            vertices_buf.append((
                float(elem.get("x", 0)),
                float(elem.get("y", 0)),
                float(elem.get("z", 0)),
            ))

        if event == "end" and tag_local(elem) == "vertices":
            objects[current_obj]["vertices"] = vertices_buf
            current_tag = None

        # ── 解析 triangles ──
        if event == "start" and t == "triangles":
            current_tag = "triangles"
            triangles_buf = []

        if event == "end" and tag_local(elem) == "triangle" and current_tag == "triangles":
            triangles_buf.append((
                int(elem.get("v1", 0)),
                int(elem.get("v2", 0)),
                int(elem.get("v3", 0)),
            ))

        if event == "end" and tag_local(elem) == "triangles":
            objects[current_obj]["triangles"] = triangles_buf
            current_tag = None

        # ── 解析 <components> ──
        if event == "start" and t == "components" and current_obj:
            objects[current_obj]["type"] = "container"

        if event == "end" and tag_local(elem) == "component":
            comp_oid = elem.get("objectid")
            transform_str = elem.get("transform", "1 0 0 0 1 0 0 0 1 0 0 0")
            transform = [float(v) for v in transform_str.split()]
            if len(transform) != 12:
                transform = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]

            # 检测 production extension: p:path 或任何 *:path 属性
            sub_path = None
            for k, v in elem.attrib.items():
                if k.endswith("}path") or k == "path":
                    sub_path = v
                    break

            comp_entry = {
                "objectid": comp_oid,
                "transform": transform,
                "sub_path": sub_path,
            }
            objects[current_obj]["components"].append(comp_entry)

        # ── 对象结束 ──
        if event == "end" and tag_local(elem) == "object":
            current_obj = None

        # ── 离开 <resources> 后停止解析 ──
        if event == "end" and tag_local(elem) == "resources":
            elem.clear()
            return objects

        if event == "end":
            elem.clear()

    return objects


def parse_all_resources(zf: zipfile.ZipFile) -> dict:
    """
    解析主模型文件及所有外部子模型文件。

    返回:
        {
            "main": {
                object_id_str: {type, vertices, triangles, components},
            },
            "external": {
                (sub_path, object_id_str): {type, vertices, triangles, components},
            }
        }
    """
    # 1. 解析主模型
    try:
        f = zf.open("3D/3dmodel.model")
        main_objects = _parse_one_model_resources(f)
        f.close()
    except KeyError:
        print("错误: 3MF 中找不到 3D/3dmodel.model", file=sys.stderr)
        sys.exit(1)

    # 2. 收集外部子文件路径
    sub_paths = set()
    for obj in main_objects.values():
        for comp in obj.get("components", []):
            if comp.get("sub_path"):
                # 去掉开头的 / 以匹配 zip 内部路径
                sp = comp["sub_path"].lstrip("/")
                sub_paths.add(sp)

    # 3. 解析每个子文件
    external_objects = {}
    for sp in sub_paths:
        try:
            f = zf.open(sp)
            sub_objs = _parse_one_model_resources(f)
            f.close()
            for oid, oobj in sub_objs.items():
                external_objects[(sp, oid)] = oobj
        except KeyError:
            print(f"  警告: 子模型文件不存在: {sp}", file=sys.stderr)
        except Exception as e:
            print(f"  警告: 解析子模型文件失败 {sp}: {e}", file=sys.stderr)

    return {"main": main_objects, "external": external_objects}


def parse_build_items(zf: zipfile.ZipFile) -> list:
    """
    解析 3dmodel.model 的 <build> 段。

    返回:
        [(objectid, transform_12_floats, printable_bool), ...]
    """
    items = []
    try:
        f = zf.open("3D/3dmodel.model")
    except KeyError:
        return items

    in_build = False
    for event, elem in safe_iterparse(f):
        if event == "start" and tag_local(elem) == "build":
            in_build = True

        if event == "end" and tag_local(elem) == "item" and in_build:
            oid = elem.get("objectid")
            transform_str = elem.get("transform", "1 0 0 0 1 0 0 0 1 0 0 0")
            printable = elem.get("printable", "1")
            transform = [float(v) for v in transform_str.split()]
            if len(transform) != 12:
                transform = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
            items.append((oid, transform, printable != "0"))

        if event == "end" and tag_local(elem) == "build":
            elem.clear()
            f.close()
            return items

        if event == "end":
            elem.clear()

    f.close()
    return items


# ── 变换矩阵 ──────────────────────────────────────────────────

def mat_mul(a, b):
    """两个 3×3 矩阵相乘。a, b 均为 12 元素 transform 数组。"""
    a33 = [[a[0], a[1], a[2]], [a[3], a[4], a[5]], [a[6], a[7], a[8]]]
    b33 = [[b[0], b[1], b[2]], [b[3], b[4], b[5]], [b[6], b[7], b[8]]]
    result = [[0.0] * 3 for _ in range(3)]
    for i in range(3):
        for j in range(3):
            result[i][j] = (
                a33[i][0] * b33[0][j]
                + a33[i][1] * b33[1][j]
                + a33[i][2] * b33[2][j]
            )
    return [
        result[0][0], result[0][1], result[0][2],
        result[1][0], result[1][1], result[1][2],
        result[2][0], result[2][1], result[2][2],
        0, 0, 0,
    ]


def transform_vertex(v, M):
    """对顶点 v=(x,y,z) 应用 12-元素变换矩阵 M。"""
    x = M[0] * v[0] + M[1] * v[1] + M[2] * v[2] + M[9]
    y = M[3] * v[0] + M[4] * v[1] + M[5] * v[2] + M[10]
    z = M[6] * v[0] + M[7] * v[1] + M[8] * v[2] + M[11]
    return (x, y, z)


# ── 对象查找 ──────────────────────────────────────────────────

def find_object(oid, all_resources, sub_path=None):
    """
    在合并的资源中查找对象。
    优先级: 主模型 > 外部子文件。
    """
    main = all_resources["main"]
    ext = all_resources["external"]

    if oid in main:
        return main[oid]

    if sub_path:
        sp = sub_path.lstrip("/")
        key = (sp, oid)
        if key in ext:
            return ext[key]

    # 回退: 在所有外部资源中搜索
    for (sp, eoid), eobj in ext.items():
        if eoid == oid:
            return eobj

    return None


def resolve_object_chain(
    root_oid: str,
    all_resources: dict,
    build_transform: list,
    sub_path: str = None,
    zf: zipfile.ZipFile = None,
) -> list:
    """
    递归解析对象引用链，展开所有 leaf mesh 对象及其累积变换矩阵。
    返回: [(vertices, triangles, final_transform), ...]
    """
    results = []

    def walk(oid, parent_transform, current_sub_path):
        obj = find_object(oid, all_resources, current_sub_path)
        if obj is None:
            print(f"  警告: 对象 {oid} 未在 resources 中定义，跳过", file=sys.stderr)
            return

        if obj["type"] == "container":
            for comp in obj.get("components", []):
                comp_oid = comp["objectid"]
                comp_xform = comp["transform"]
                comp_sub = comp.get("sub_path")  # 子文件中查找
                chain_transform = mat_mul(parent_transform, comp_xform)
                walk(comp_oid, chain_transform, comp_sub)
        else:
            if obj["vertices"] and obj["triangles"]:
                results.append((obj["vertices"], obj["triangles"], parent_transform))
            else:
                print(f"  警告: 对象 {oid} 无 mesh 数据，跳过", file=sys.stderr)

    walk(root_oid, build_transform, sub_path)
    return results


# ── STL 输出 ──────────────────────────────────────────────────

def compute_normal(v1, v2, v3):
    """计算三角形面法向量 (cross product, normalize)。"""
    e1 = (v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2])
    e2 = (v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2])
    nx = e1[1] * e2[2] - e1[2] * e2[1]
    ny = e1[2] * e2[0] - e1[0] * e2[2]
    nz = e1[0] * e2[1] - e1[1] * e2[0]
    length = (nx * nx + ny * ny + nz * nz) ** 0.5
    if length < 1e-12:
        return (0.0, 0.0, 1.0)
    return (nx / length, ny / length, nz / length)


def write_stl(filepath, name, vertices, triangles, transform):
    """写入二进制 STL 文件。"""
    transformed = [transform_vertex(v, transform) for v in vertices]

    valid_tris = []
    for v1_idx, v2_idx, v3_idx in triangles:
        p1 = transformed[v1_idx]
        p2 = transformed[v2_idx]
        p3 = transformed[v3_idx]
        if p1 != p2 and p2 != p3 and p1 != p3:
            valid_tris.append((v1_idx, v2_idx, v3_idx))

    with open(filepath, "wb") as f:
        header = name[:80].encode("ascii", errors="replace").ljust(80, b"\x00")
        f.write(header)
        f.write(struct.pack("<I", len(valid_tris)))

        for v1_idx, v2_idx, v3_idx in valid_tris:
            p1 = transformed[v1_idx]
            p2 = transformed[v2_idx]
            p3 = transformed[v3_idx]
            n = compute_normal(p1, p2, p3)
            f.write(struct.pack(
                "<3f 3f 3f 3f H",
                n[0], n[1], n[2],
                p1[0], p1[1], p1[2],
                p2[0], p2[1], p2[2],
                p3[0], p3[1], p3[2],
                0,
            ))


# ── ZIP 打包 ──────────────────────────────────────────────────

def pack_zip(zip_basename, stl_files, output_dir):
    """将所有 STL 打包为 <zip_basename>.zip。"""
    zip_path = os.path.join(output_dir, f"{zip_basename}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for stl_path, arcname in stl_files:
            zf.write(stl_path, arcname)
    return zip_path


def safe_filename(name):
    """移除文件名中的非法字符。"""
    safe = "".join(c for c in name if c not in r'<>:"/\|?*').strip()
    return safe or "unnamed"


def unique_name(base, used_names):
    """在 base 后追加 _N 直到不重复。"""
    if base not in used_names:
        used_names.add(base)
        return base
    i = 2
    while f"{base}_{i}" in used_names:
        i += 1
    name = f"{base}_{i}"
    used_names.add(name)
    return name


# ── 主流程 ────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="从 3MF 文件中提取模型并导出为 STL (ZIP 打包)",
    )
    parser.add_argument("input", help="输入的 .3mf 文件路径")
    parser.add_argument(
        "-o", "--output-dir",
        default=None,
        help="输出目录（默认与输入文件同目录）",
    )
    parser.add_argument(
        "--score", action="store_true",
        help="在输出末尾打印切片复杂度评分",
    )
    args = parser.parse_args()

    input_path = args.input
    if not os.path.isfile(input_path):
        print(f"错误: 文件不存在: {input_path}", file=sys.stderr)
        sys.exit(1)

    output_dir = args.output_dir or os.path.dirname(input_path) or "."
    os.makedirs(output_dir, exist_ok=True)

    print(f"读取: {input_path}")

    with zipfile.ZipFile(input_path, "r") as zf:
        # ── 1. 提取命名信息 ──
        title = parse_title(zf)
        model_names = parse_model_settings(zf)
        input_basename = os.path.splitext(os.path.basename(input_path))[0]

        if title:
            zip_basename = safe_filename(title)
        else:
            zip_basename = safe_filename(input_basename) or "model"

        print(f"  标题: {repr(title)}")
        print(f"  ZIP 名: {zip_basename}")

        # ── 2. 解析所有 model（包括生产扩展子文件）──
        print("解析模型结构...")
        all_resources = parse_all_resources(zf)
        total_objs = len(all_resources["main"]) + len(all_resources["external"])
        print(f"  共 {total_objs} 个对象 "
              f"(主模型: {len(all_resources['main'])}, "
              f"子文件: {len(all_resources['external'])})")

        # ── 3. 解析 build ──
        build_items = parse_build_items(zf)
        print(f"  build 中有 {len(build_items)} 个 item")

    # ── 4. 展开对象链并写入 STL ──
    with tempfile.TemporaryDirectory() as tmpdir:
        stl_files = []
        used_stl_names = set()
        total_tri_count = 0  # 用于 --score

        for item_idx, (oid, transform, printable) in enumerate(build_items):
            if not printable:
                print(f"  跳过不可打印的 item (objectid={oid})")
                continue

            leaf_objects = resolve_object_chain(oid, all_resources, transform)

            for leaf_idx, (vertices, triangles, final_transform) in enumerate(leaf_objects):
                # 命名
                obj_name = model_names.get(oid, f"object_{oid}")
                if len(leaf_objects) > 1:
                    obj_name = f"{obj_name}_{leaf_idx + 1}"

                safe_name = safe_filename(obj_name)
                if not safe_name:
                    safe_name = f"object_{oid}"
                # 去重
                safe_name = unique_name(safe_name, used_stl_names)

                stl_path = os.path.join(tmpdir, f"{safe_name}.stl")
                tri_count = len(triangles)
                total_tri_count += tri_count
                print(f"  写入: {safe_name}.stl "
                      f"({len(vertices)} 顶点, {tri_count} 三角形)")

                write_stl(stl_path, safe_name, vertices, triangles, final_transform)

                arcname = f"{zip_basename}/{safe_name}.stl"
                stl_files.append((stl_path, arcname))

        if not stl_files:
            print("错误: 没有可导出的模型（所有 item 不可打印或无 mesh）", file=sys.stderr)
            sys.exit(1)

        # ── 5. 打包 ZIP ──
        zip_path = pack_zip(zip_basename, stl_files, output_dir)

    print(f"\n完成: {zip_path}")
    print(f"  ZIP 内文件:")
    for _, arcname in stl_files:
        print(f"    {arcname}")

    # ── 6. 复杂度评分（--score 时输出）──
    if args.score:
        if _HAS_SCORER:
            result = score_model(total_tri_count)
            s = result["score"]
            bar_len = 20
            filled = round(s / 20 * bar_len)
            bar = "█" * filled + "░" * (bar_len - filled)
            print(f"\n{'─' * 54}")
            print(f"复杂度评分: {s}/20  [{bar}]  ({total_tri_count:,} 面片)")
            print(f"等级: {result['level']} — {result['level_desc']}")
            print(f"{'─' * 54}")
        else:
            print(f"\n复杂度: {total_tri_count:,} 面片 (安装 model_complexity.py 获取评分)")


if __name__ == "__main__":
    main()
