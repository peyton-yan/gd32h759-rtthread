#!/usr/bin/env bash
#
# bootstrap.sh — 拉取 RT-Thread 上游并叠加本项目增量
#
#   git clone rt-thread@TAG -> 覆盖 overlay/ -> 应用 patches/ -> 提示后续步骤
#
set -euo pipefail

RT_TAG="${RT_TAG:-v5.2.2}"
RT_REPO="${RT_REPO:-https://github.com/RT-Thread/rt-thread.git}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RTT="$ROOT/rt-thread"
PATCH="$ROOT/patches/upstream-${RT_TAG}.patch"

cd "$ROOT"

# 1) 上游
if [ ! -d "$RTT" ]; then
    echo ">> clone rt-thread $RT_TAG"
    git clone --depth 1 --branch "$RT_TAG" "$RT_REPO" "$RTT"
else
    echo ">> rt-thread 已存在，跳过 clone"
fi

# 2) 覆盖新增文件
echo ">> overlay -> rt-thread/"
cp -a "$ROOT/overlay/." "$RTT/"

# 3) 打补丁（对上游原生文件的改动）
if [ -f "$PATCH" ]; then
    echo ">> apply $PATCH"
    git -C "$RTT" apply --whitespace=nowarn "$PATCH"
else
    echo "!! 未找到 patch：$PATCH"
fi

echo ""
echo "==================================================================="
echo "下一步："
echo "  cd rt-thread/bsp/gd32/arm/gd32h759i-eval"
echo "  pkgs --update        # 获取 GD32 SDK / CMSIS（第三方，需 RT-Thread env）"
echo "  scons -j8            # 产物 rtthread.elf / .bin / .map"
echo ""
echo "上位机（在仓库根的 host/，与设备树分开）："
echo "  cd host"
echo "  npm install && npm run sync-points && npm run tauri dev"
echo "==================================================================="
