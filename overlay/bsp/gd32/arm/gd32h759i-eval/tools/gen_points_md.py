#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_points_md.py — 从 tools/points.json 生成 Markdown 点表（博客附录用）。

链路：applications/mb_points.c --(gen_points_json.py)--> tools/points.json
      --(本脚本)--> tools/docs/POINTS_TABLE.md
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "points.json")
OUT = os.path.join(HERE, "docs", "POINTS_TABLE.md")

AREA_ORDER = ["coil", "discrete_input", "input_register", "holding_register"]


def main():
    if not os.path.isfile(SRC):
        sys.stderr.write("points.json not found: %s\n" % SRC)
        return 1

    with open(SRC, encoding="utf-8") as f:
        data = json.load(f)

    lines = []
    lines.append("# 设备点表（全量）\n")
    lines.append("> 设备：**%s** · 共 **%d** 点（当前编入 **%d**）\n"
                 % (data.get("device"), data.get("total"), data.get("enabled_total")))
    lines.append("> 来源：`applications/mb_points.c` → `tools/points.json` → 本表（由 `tools/gen_points_md.py` 生成）\n")

    areas = data.get("areas", {})
    order = [k for k in AREA_ORDER if k in areas] + [k for k in areas if k not in AREA_ORDER]

    for key in order:
        info = areas[key]
        fcs = " / ".join("0x%02X" % fc for fc in info.get("fc", []))
        pts = info.get("points", [])
        lines.append("\n## %s（%s，FC %s）\n" % (info.get("label", key), info.get("prefix", ""), fcs))
        lines.append("| 点名 | 地址 | 参考号 | 类型 | 读写 | 绑定 | 启用 |")
        lines.append("|---|---|:---:|---|---|---|---|")
        for p in pts:
            lines.append("| `%s` | `%s` | %d | %s | %s | %s | %s |" % (
                p["name"], p["addr_hex"], p["ref"], p["type"], p["attr"], p["bind"],
                "✓" if p.get("enabled") else "—"))
        lines.append("")

    lines.append("\n---\n")
    lines.append("> 说明：**参考号**为 Modbus 常用表示（0x→00001、1x→10001、3x→30001、4x→40001 起）；"
                 "上位机实际使用**协议 0 基地址**（即 `地址` 列的 `0x....`）。\n")
    lines.append("> 类型：`bit` 位 / `u16` / `s16` 有符号 / `u32` 占连续 2 寄存器（高字在前）；"
                 "`绑定`为 `reg`（变量）或 `hook`（回调）。\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d points)" % (OUT, data.get("total", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
