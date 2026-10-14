# GD32H759 + RT-Thread 工控实战（设备 + 上位机 + 闭环控制）

> 基于 **GD32H759I-EVAL + RT-Thread v5.2.2** 的一套可跑通的"工控"示例：
> **Modbus TCP 全量点表**（设备侧）+ **Tauri/Svelte 上位机**（PC 侧）+ **DAC↔ADC 闭环 PID**。
>
> 本仓库**只包含本项目自己的增量**，不重复托管 RT-Thread（见 `scripts/bootstrap.sh`）。

<!-- 顶部演示动图：拖滑块 → 板上响应 / 闭环收敛 -->
![demo](docs/images/demo.gif)

---

## ✨ 特性

| 能力 | 说明 |
|---|---|
| 表驱动点表 | 一个 `MB_PT(...)` = 一个点；点表是**单一事实来源** |
| 自动界面 | 上位机读 `points.json` **自动生成**全部面板，前后端不重复描述 |
| 全量点位 | **72 点**（系统/LED/按键/ADC/DAC/RTC/EEPROM/Flash/SD/CAN/音频/USB/ENET/**闭环**） |
| 闭环控制 | DAC↔ADC **整数 PID**（100Hz）+ **越限联锁**（切安全输出）+ 实时性指标（扫描周期/抖动） |
| 上位机 | **Tauri v2 + Svelte**；实时趋势（多序列独立缩放）、限值告警、CSV 导出 |
| M7 稳定性 | **DMA 专用非cacheable 区**（MPU）+ 池分配器，规避 D-Cache 踩内存 |
| 自检 | 设备端 `mb_selftest` 本地校验 Modbus 帧，无需 PC |

---

## 🚀 一键运行

> 前置：Linux/WSL，`git`、`arm-none-eabi-gcc`、`scons`、RT-Thread `env`（用于 `pkgs`）。
> PC 上位机另需 `Node 18+`、`Rust`、Tauri v2 系统依赖。

```bash
git clone https://github.com/<you>/gd32h759-rtthread-industrial.git
cd gd32h759-rtthread-industrial
./scripts/bootstrap.sh          # 拉取 RT-Thread v5.2.2 并覆盖/打补丁

cd rt-thread/bsp/gd32/arm/gd32h759i-eval
pkgs --update                   # 获取 GD32 SDK / CMSIS（第三方，不入库）
scons -j8                       # 产物：rtthread.elf / .bin / .map
```

上位机（在仓库根的 `host/`，与设备树分开）：
```bash
cd host
npm install && npm run sync-points && npm run tauri dev
```

---

## 🧱 目录结构

```
gd32h759-rtthread-industrial/
├─ scripts/
│   ├─ bootstrap.sh      # clone 上游 + overlay + patch
│   └─ collect.sh        # 从开发树自动收集 overlay/ 与 patch
├─ patches/
│   └─ upstream-v5.2.2.patch   # 对 RT-Thread 原生文件的改动
├─ host/                 # PC 上位机（Tauri + Svelte，独立于设备树）
├─ points.json           # 点表（上位机 sync-points 读取；设备侧生成）
├─ overlay/              # 设备侧新增文件（按 RT-Thread 内路径存放）
│   └─ bsp/gd32/arm/...
├─ docs/                 # 文档（点表/跳线/DMA 布局/联调/博客提纲/开源方案）
├─ LICENSE               # 自研部分：Apache-2.0
└─ NOTICE                # 上游归属与改动声明
```

---

## 🧭 改动落点约定

贡献 / 维护时请按此归类。口诀：**新增 → overlay，改上游 → patch，改包 → 搬进 BSP，第三方 → pkgs。**

| 改动类型 | 放在哪 | 说明 |
|---|---|---|
| BSP 新增源码（应用/板级/工具/自研驱动） | `overlay/bsp/...` | 按 RT-Thread 内原路径存放 |
| **上游原生文件**的改动 | `patches/upstream-v5.2.2.patch` | 由 `scripts/collect.sh` 生成；bootstrap 自动 `git apply` |
| **pkgs 包**（如 GD32 库）的改动 | 搬进 `overlay/.../board/hal_supplement/` | **不改包**，包保持原样（详见该目录 `README.md`） |
| 上位机 | `host/` | 独立于设备树 |
| 点表 | `points.json`（仓库根） | 设备侧生成 |
| 第三方 SDK / CMSIS | 不入库 | 由 `pkgs --update` 获取 |

> 改完代码后跑一次 `scripts/collect.sh` 刷新 `overlay/` 与 `patches/`，再提交。

---

## 📊 文档

- [点表全表](docs/POINTS_TABLE.md)
- [跳线表](docs/JUMPERS.md)
- [DMA / Cache / MPU 布局](docs/DMA_LAYOUT.md)
- [上板联调清单](docs/COMMISSIONING.md)

---

## 🔌 硬件

- 板卡：GD32H759I-EVAL（GD32H759IMK6，Cortex-M7 @600MHz）
- 闭环演示接线：`JP47 → DAC`，杜邦线 **PA5 ↔ PC2_C**，板载电位器作负载
- 详见 [跳线表](docs/JUMPERS.md)

---

## 📜 许可

- 本项目自研代码：**Apache-2.0**（见 `LICENSE`）。
- RT-Thread：Apache-2.0，版权归 RT-Thread 团队；本仓库通过 **patch** 修改其少量文件，改动清单见 `NOTICE`。
- GD32 标准外设库 / CMSIS：**第三方**，由 `pkgs` 获取，未包含在本仓库，遵从各自许可。
