#!/usr/bin/env bash
#
# collect.sh — 从本地 RT-Thread 开发树收集"本项目增量"到本仓库
#
#   patches/upstream-<tag>.patch  上游原生文件的改动
#   overlay/                      设备侧新增文件（按 RT-Thread 内路径）
#   host/                         PC 上位机（顶层独立目录，**不下沉到设备树**）
#   points.json                   点表（仓库根，供上位机 sync-points 使用）
#   docs/                         文档（便于根目录阅读）
#
# 用法： scripts/collect.sh [SRC_ROOT] [TAG]
#
set -euo pipefail

SRC="${1:-/home/yan/gd32/rt-thread}"
TAG="${2:-v5.2.2}"
DEV="bsp/gd32/arm/gd32h759i-eval"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 不收集：第三方库、构建产物、系统残留、本仓库自身、AGENTS.md、
#         以及上位机目录（它单独放到顶层 host/）
EXCL='gd32-arm-series-latest|gd32-arm-cmsis-latest|__pycache__|Zone\.Identifier'
EXCL="$EXCL"'|(^|/)open-source/|(^|/)\.git/'
EXCL="$EXCL"'|(^|/)AGENTS\.md$'
EXCL="$EXCL"'|(^|/)'"$DEV"'/tools/host/'
EXCL="$EXCL"'|\.(o|o\.d|elf|axf|hex|bin|map|dblite|pyc|a)$'

echo ">> SRC = $SRC"
echo ">> OUT = $OUT"

mkdir -p "$OUT/patches"

# ---- 1) 上游改动 -> patches/ ----
git -C "$SRC" diff --name-only | grep -vE "$EXCL" > /tmp/os_mod.txt || true
if [ -s /tmp/os_mod.txt ]; then
    ( cd "$SRC" && git diff -- $(cat /tmp/os_mod.txt) ) > "$OUT/patches/upstream-${TAG}.patch"
    echo ">> patch: $(wc -l < /tmp/os_mod.txt) 个文件"
else
    echo ">> patch: （无上游改动）"
fi

# ---- 2) 设备侧新增文件 -> overlay/ ----
rm -rf "$OUT/overlay"
mkdir -p "$OUT/overlay"
git -C "$SRC" ls-files --others --exclude-standard \
    | grep -vE "$EXCL" \
    | while read -r f; do
          mkdir -p "$OUT/overlay/$(dirname "$f")"
          cp -a "$SRC/$f" "$OUT/overlay/$f"
      done
echo ">> overlay: $(find "$OUT/overlay" -type f | wc -l) 个文件"

# ---- 3) 上位机 -> 顶层 host/ ----
HOST_SRC="$SRC/$DEV/tools/host"
if [ -d "$HOST_SRC" ]; then
    rm -rf "$OUT/host"
    mkdir -p "$OUT/host"
    ( cd "$HOST_SRC" && tar --exclude=node_modules --exclude=dist \
          --exclude=src-tauri/target --exclude=src/lib/points.json -cf - . ) \
        | ( cd "$OUT/host" && tar -xf - )
    echo ">> host: $(find "$OUT/host" -type f | wc -l) 个文件"
fi

# ---- 4) 点表 -> 仓库根（上位机 sync-points 读 ../../points.json） ----
if [ -f "$SRC/$DEV/tools/points.json" ]; then
    cp -a "$SRC/$DEV/tools/points.json" "$OUT/points.json"
    echo ">> points.json"
fi

# ---- 5) 文档 -> 根 docs/（仅公开发布的；internal/ 与本索引不发布） ----
DOCS_SRC="$SRC/$DEV/tools/docs"
if [ -d "$DOCS_SRC" ]; then
    mkdir -p "$OUT/docs"
    # 清掉旧的根级 .md（避免历史遗留的内部文档残留；images/ 目录保留）
    find "$OUT/docs" -maxdepth 1 -type f -name '*.md' -delete
    rm -rf "$OUT/docs/internal"
    ( cd "$DOCS_SRC" && tar --exclude=./internal --exclude=./README.md -cf - . ) \
        | ( cd "$OUT/docs" && tar -xf - )
    echo ">> docs: $(find "$OUT/docs" -type f | wc -l) 个文件"
fi

echo ">> 完成。"
