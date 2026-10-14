#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_points_json.py — 从 applications/mb_points.c 抽取 MB_PT(...) 点表，
导出 tools/points.json 供 PC 上位机（Tauri/Svelte）自动生成界面。

约定：mb_points.c 中每个点必须写成单行的
    MB_PT(area, addr, type, attr, reg, hook, "NAME"),

脚本还会读取 rtconfig.h，按点表里的 `#if defined(...)` 编译期开关，
为每个点标注 enabled（该外设当前是否编入固件）。
"""
import json
import re
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "applications", "mb_points.c"))
RTCONFIG = os.path.normpath(os.path.join(HERE, "..", "rtconfig.h"))
OUT = os.path.join(HERE, "points.json")

# 顺序即输出顺序；(key, json名, 中文, 前缀, 功能码, 参考号基址)
AREAS = [
    ("MB_AREA_COIL", "coil",             "线圈",       "0x", [1, 5, 15],   1),
    ("MB_AREA_DIN",  "discrete_input",   "离散输入",   "1x", [2],       10001),
    ("MB_AREA_IREG", "input_register",   "输入寄存器", "3x", [4],       30001),
    ("MB_AREA_HREG", "holding_register", "保持寄存器", "4x", [3, 6, 16], 40001),
]
TYPE = {"MB_T_BIT": "bit", "MB_T_U16": "u16", "MB_T_S16": "s16",
        "MB_T_U32": "u32", "MB_T_STR": "str"}
ATTR = {"MB_A_RO": "r", "MB_A_RW": "rw", "MB_A_WO": "w"}

PAT = re.compile(
    r'MB_PT\(\s*([A-Z_0-9]+)\s*,\s*(0[xX][0-9A-Fa-f]+)\s*,\s*([A-Z_0-9]+)\s*,'
    r'\s*([A-Z_0-9]+)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*"([^"]+)"\s*\)'
)
DEFINE_PAT = re.compile(r'#define\s+(\w+)')


def load_defines(path):
    defs = set()
    if os.path.isfile(path):
        with open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = DEFINE_PAT.match(line.strip())
                if m:
                    defs.add(m.group(1))
    return defs


def eval_if(cond, defs):
    macros = re.findall(r'defined\(\s*(\w+)\s*\)', cond)
    if not macros:
        return True
    if "||" in cond:
        return any(m in defs for m in macros)
    return all(m in defs for m in macros)


def main():
    if not os.path.isfile(SRC):
        sys.stderr.write("source not found: %s\n" % SRC)
        return 1

    defs = load_defines(RTCONFIG)
    buckets = {k: [] for k, *_ in AREAS}
    ref_base = {a[0]: a[5] for a in AREAS}
    stack = []

    with open(SRC, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            s = line.strip()
            if s.startswith("#if"):
                stack.append(eval_if(s, defs))
                continue
            if s.startswith("#else"):
                if stack:
                    stack[-1] = not stack[-1]
                continue
            if s.startswith("#endif"):
                if stack:
                    stack.pop()
                continue

            m = PAT.search(line)
            if not m:
                continue
            area, addr, typ, attr, reg, hook, name = m.groups()
            if area not in buckets:
                sys.stderr.write("line %d: unknown area %s\n" % (lineno, area))
                continue
            reg, hook = reg.strip(), hook.strip()
            enabled = all(stack) if stack else True
            a = int(addr, 16)
            buckets[area].append({
                "name": name,
                "addr": a,
                "addr_hex": "0x%04X" % a,
                "ref": ref_base[area] + a,
                "type": TYPE.get(typ, typ),
                "attr": ATTR.get(attr, attr),
                "bind": "hook" if hook != "RT_NULL" else ("reg" if reg != "RT_NULL" else "none"),
                "impl": (reg != "RT_NULL") or (hook != "RT_NULL"),
                "enabled": enabled,
            })

    out = {"device": "GD32H759I-EVAL", "generated_from": "applications/mb_points.c", "areas": {}}
    total = enabled_total = 0
    for key, json_name, label, prefix, fc, _base in AREAS:
        pts = sorted(buckets[key], key=lambda p: p["addr"])
        total += len(pts)
        enabled_total += sum(1 for p in pts if p["enabled"])
        out["areas"][json_name] = {"label": label, "prefix": prefix, "fc": fc, "points": pts}
    out["total"] = total
    out["enabled_total"] = enabled_total

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print("wrote %s (%d points, %d enabled)" % (OUT, total, enabled_total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
