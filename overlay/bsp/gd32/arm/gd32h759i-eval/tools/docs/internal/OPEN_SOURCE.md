# 开源方案：不重复托管 RT-Thread

> 目标：把**本项目自己的增量**（BSP + 自研驱动 + 应用 + 上位机 + 文档）开源到 GitHub，
> **不把整个 RT-Thread 再传一遍**，且保证别人能**一条命令复现构建**。

---

## 一、核心思路：Overlay（覆盖层）仓库 + 引导脚本

RT-Thread 是 Apache-2.0、体量巨大，**重复托管它既冗余又难同步上游**。
本项目的代码天然是"贴在 RT-Thread 树上的一层"，所以最合适的形态是：

```
[pinned] rt-thread @ v5.2.2  (脚本自动 clone，不进你的仓库)
        +
[你的仓库] overlay/  (新增文件，按原路径存放)
           patches/  (对上游文件的改动，.patch)
           scripts/bootstrap.sh  (clone + 覆盖 + 打补丁 + 准备配置)
```

别人只要：
```bash
git clone <你的仓库> gd32h759-rtthread
cd gd32h759-rtthread
./scripts/bootstrap.sh        # 拉 RT-Thread v5.2.2 并覆盖/打补丁
cd rt-thread/bsp/gd32/arm/gd32h759i-eval && scons -j8
```

---

### 改动落点总览（一句话规则：新增→overlay，改上游→patch，改包→搬进 BSP，第三方→pkgs）

| 改动类型 | 开发树中的实际位置 | 受本工程 git？ | 进开源仓库的方式 | 复现方式 |
|---|---|---|---|---|
| BSP 新增源码（应用/板级/工具） | `bsp/.../gd32h759i-eval/{applications,board,tools}` | ❌ 未跟踪 | **`overlay/`**（按原路径） | bootstrap 覆盖 |
| 自研驱动 | `bsp/gd32/arm/libraries/gd32_drivers/*` | ❌ 未跟踪 | **`overlay/`** | bootstrap 覆盖 |
| 上位机（Tauri） | `bsp/.../tools/host` | ❌ 未跟踪 | **`host/`**（顶层独立） | `npm i && npm run sync-points && npm run tauri dev` |
| 点表 | `bsp/.../tools/points.json` | ❌ 未跟踪 | **`points.json`**（仓库根） | 设备侧 `gen_points_json.py` 后同步 |
| 文档 | `bsp/.../tools/docs` | ❌ 未跟踪 | **`docs/`**（根）＋ overlay 内 | `collect.sh` 复制 |
| **上游原生文件改动** | `board/{SConscript,Kconfig}`、`.config`、`rtconfig.h`、`applications/main.c`、`libraries/gd32_drivers/{SConscript,drv_can.c,drv_gpio.c}`、`components/**`、`src/Kconfig` | ✅ 已跟踪 | **`patches/upstream-<tag>.patch`** | bootstrap `git apply` |
| **pkgs 包改动** | `libraries/gd32-arm-series-latest`（**独立 git 仓库**） | ❌（包自己的 git） | **搬到 `board/hal_supplement/`** → overlay；包**还原原样** | bootstrap 覆盖（无需 patch 包） |
| 第三方 SDK / CMSIS | `libraries/gd32-arm-{cmsis,series}-latest` | — | **不入库** | `pkgs --update` |

> 判断口诀：**这份改动是不是"新增文件"？** 是 → `overlay/`；
> **是不是改了 RT-Thread 自带文件？** 是 → `patches/`；
> **是不是在 pkgs 拉下来的包里？** 是 → 搬进 BSP；
> **是第三方库本身？** → 交给 `pkgs`，不入库。

---

## 二、仓库目录建议

```
gd32h759-rtthread/                 # 你的开源仓库（建议命名）
├─ README.md                       # 项目介绍 + 架构图 + 一键运行 + 截图/动图
├─ LICENSE                         # 你自研部分的许可（建议 Apache-2.0 或 MIT）
├─ NOTICE                          # 上游归属与改动声明（见"许可"节）
├─ .gitignore
├─ docs/                           # 与设备侧 tools/docs 同步的文档（可直接软链/拷贝）
│   ├─ POINTS_TABLE.md
│   ├─ DMA_LAYOUT.md
│   ├─ JUMPERS.md
│   ├─ COMMISSIONING.md
│   └─ BLOG_OUTLINE_10.md
├─ overlay/                        # 新增文件：**按 RT-Thread 内的相对路径**存放
│   └─ bsp/gd32/arm/
│       ├─ gd32h759i-eval/
│       │   ├─ applications/*.c,.h        # 自研 app（含 mb_*、app_*、测试）
│       │   ├─ board/fal_cfg.h usb_config.h
│       │   ├─ tools/                     # 点表脚本/文档/上位机（见下）
│       │   ├─ rtconfig.h  .config  Kconfig(改动?) — 见 patches 说明
│       └─ libraries/gd32_drivers/*.c,.h  # 自研驱动
├─ patches/
│   └─ upstream-v5.2.2.patch        # 对 RT-Thread 原生文件的全部改动
├─ scripts/
│   ├─ bootstrap.sh                 # clone + overlay + patch + 校验
│   └─ collect.sh                   # 从开发仓库自动生成 overlay/ 与 patch
└─ host/                            # 上位机（Tauri），也可拆到独立仓库
```

> **为什么新增文件走 `overlay/`、上游改动走 `patches/`**：
> - 新增文件（`mb_*.c`、`app_*.c`、`drv_*_h7.c`…）直接放，清晰、可 diff；
> - 上游原生文件（`src/Kconfig`、`components/...` 等）用 patch，**不复制上游源码**，
>   既避免许可纠缠，也方便上游升级时重放。

---

## 三、要纳入的内容（本项目增量清单）

### 3.1 新增文件（`overlay/`）

| 类别 | 路径 | 说明 |
|---|---|---|
| 应用 | `bsp/gd32/arm/gd32h759i-eval/applications/*` | `mb_*`、`app_*`、`can_test`、`adc_dac_test` … |
| 板级 | `bsp/gd32/arm/gd32h759i-eval/board/{fal_cfg.h,usb_config.h}` | 分区表、USB 配置 |
| 自研驱动 | `bsp/gd32/arm/libraries/gd32_drivers/drv_*_h7.c`、`gd25x512me.*` 等 | 见 `git status` 的 `??` 列表 |
| 文档/工具 | `bsp/gd32/arm/gd32h759i-eval/tools/**` | 点表脚本、`docs/`、`host/` |
| 台账 | `AGENTS.md` | 可选开源 |

### 3.2 上游改动（`patches/upstream-v5.2.2.patch`）

当前对 RT-Thread 原生文件的改动（`git status` 的 `M`）：

```
bsp/gd32/arm/gd32h759i-eval/{.config, rtconfig.h, board/Kconfig, applications/main.c}
bsp/gd32/arm/libraries/gd32_drivers/{SConscript, drv_can.c, drv_gpio.c}
components/drivers/sdio/{Kconfig, dev_mmcsd_core.c, dev_sd.c}
components/drivers/serial/dev_serial.c
components/drivers/usb/cherryusb/core/usbd_core.c
components/drivers/usb/cherryusb/demo/cdc_acm_rttchardev_template.c
components/drivers/usb/cherryusb/port/dwc2/{usb_dc_dwc2.c, usb_dwc2_param.h, usb_glue_gd.c}
src/Kconfig
```

> 注意：`rtconfig.h`/`.config` 是**生成物**。更好的做法是：patch 只带 `.config`（并用
> `scons --pyconfig` / `menuconfig` 生成 `rtconfig.h`），或直接带二者并在 README 说明。
> `main.c` 若只是加/删示例，按 patch 处理。

### 3.3 **不纳入**（由使用者自行获取）

- `bsp/gd32/arm/libraries/gd32-arm-series-latest/`（GigaDevice 标准外设库，**第三方**）
- `bsp/gd32/arm/libraries/gd32-arm-cmsis-latest/`（CMSIS，**第三方**）
- `build/`、`*.o`、`*.elf/bin/map`、`.sconsign.dblite`、`__pycache__/`
- 上位机的 `node_modules/`、`dist/`、`src-tauri/target/`

这两个第三方库通过 RT-Thread 的 **pkgs** 获取（`pkgs --update`），README 里给出步骤与版本。

### 3.4 包（pkgs）的改动：**不直接改包**

`pkgs --update` 拉下来的库（如 `libraries/gd32-arm-series-latest`）是**独立 git 仓库**，
不在本工程 git 之下——直接改它，本仓库的 overlay/patch **收不到**，且可能被 `pkgs` 覆盖。

本项目的做法（详见 `board/hal_supplement/README.md`）：

1. 把需要补充/改动的源文件**搬进 BSP**（受本工程 git 管理，如 `board/hal_supplement/*.c`）；
2. 在 BSP 侧用 `SConscript` 编译它们（**相对路径**引入以继承 variant 映射，对象落 `build/`）；
3. 把包内文件**还原为上游原样**；
4. 迁移记录存档为 `board/hal_supplement/migrated-from-pkg.patch`。

> 本次实例：包内 `GD32H7xx/SConscript` 原被补入 `dci/ospi/ospim/timer/trigsel/hpdf` 六个源文件；
> 已还原该文件，六者移到 `board/hal_supplement/`（其中 `hpdf` 顺带升级到 V1.4.0）。

---

## 四、`scripts/bootstrap.sh` 做什么

```bash
#!/usr/bin/env bash
set -euo pipefail
RT_TAG="${RT_TAG:-v5.2.2}"           # 固定上游版本，保证可复现
RT_REPO="${RT_REPO:-https://github.com/RT-Thread/rt-thread.git}"

# 1) 拉取上游（不进你的仓库）
[ -d rt-thread ] || git clone --depth 1 --branch "$RT_TAG" "$RT_REPO" rt-thread

# 2) 覆盖新增文件
cp -av overlay/. rt-thread/

# 3) 应用对上游文件的改动
git -C rt-thread apply --whitespace=nowarn ../patches/upstream-${RT_TAG}.patch

# 4) 取第三方依赖（GD32 SDK/CMSIS）
cd rt-thread/bsp/gd32/arm/gd32h759i-eval
#   （用 RT-Thread env：pkgs --update 或 scons --menuconfig 后 pkgs --update）
echo "Next: menuconfig -> pkgs --update -> scons -j8"
```

> 版本 pin 是关键：把 `RT_TAG`（或具体 commit）写死在仓库里，
> 上游升级时只需重放 patch（`git -C rt-thread am`）。

---

## 五、`scripts/collect.sh` —— 从开发仓库一键收集

把当前 RT-Thread 工作区里的"我方增量"自动整理成 `overlay/` + `patches/`：

```bash
#!/usr/bin/env bash
set -euo pipefail
SRC=/home/yan/gd32/rt-thread                 # 你的开发目录
OUT=$(pwd)

# 上游改动 -> patch
git -C "$SRC" diff --name-only > /tmp/mod.txt
git -C "$SRC" diff $(cat /tmp/mod.txt) > "$OUT/patches/upstream-v5.2.2.patch"

# 新增文件 -> overlay（排除第三方库与产物）
git -C "$SRC" ls-files --others --exclude-standard \
  | grep -vE 'gd32-(arm-cmsis|arm-series)-latest|/build/|\.(o|elf|bin|map|dblite|pyc)$|Zone.Identifier' \
  | while read -r f; do mkdir -p "$OUT/overlay/$(dirname "$f")"; cp -a "$SRC/$f" "$OUT/overlay/$f"; done
```

> 注意：`.config`/`rtconfig.h` 是 `M`（改动），会被打进 patch；`tools/`、`applications/` 是 `??`（新增），走 overlay。

---

## 六、许可与署名（重要）

- RT-Thread 及其修改文件：**保持 Apache-2.0**，并在 `NOTICE` 里**声明你改动了哪些文件**（Apache-2.0 的要求）。
- GD32 SDK / CMSIS：**第三方**，不要并入你的仓库、不要重新授权；README 指向官方获取方式。
- 你自研的 `applications/`、`drv_*_h7.c`、`host/`、`tools/`：可自行选择（建议 **Apache-2.0** 与上游一致，或 MIT）。
- 仓库根 `LICENSE` 放你自研部分的许可；`NOTICE` 写清混合许可的范围。

---

## 七、发布物（Release）

一次 Release 建议附带：

- `rtthread.bin`（固件）、`points.json`（点表）、`docs/`（打包）
- `host` 的免安装包（Tauri `bundle`，Windows `.msi`）
- **30–60 秒演示视频/GIF**（放 README 顶部）
- `CHANGELOG.md`

---

## 八、方案对比（为什么选 overlay）

| 方案 | 优点 | 缺点 | 适用 |
|---|---|---|---|
| **Overlay + patch（推荐）** | 仓库小、可复现、上游可升级 | 需要 bootstrap 脚本 | ✅ 贴树开发 |
| 只发 patch | 最小 | 不能直接构建、结构不清 | 补丁型改动 |
| Fork rt-thread | 最简单 | **要上传整棵树**、难同步 | ❌（你明确不要） |
| submodule 嵌 RT-Thread | 版本清晰 | 你的代码仍散在子模块里，`git status` 混 | 一般 |
| subtree | 单仓库 | 历史臃肿、易冲突 | ❌ |

---

## 九、发布到 GitHub（正确步骤）

> ⚠️ **绝对不要在 RT-Thread 树里 `git add/commit`。**
> `~/gd32/rt-thread` 是**上游 RT-Thread 的 clone**，你只在里面"改文件 + 跑脚本"。
> 一旦在那里 `git add -A`，本项目文件会变成"已暂存/已跟踪"，
> `collect.sh` 的 `git diff`（未暂存）与 `git ls-files --others`（未跟踪）就**收不到它们**，
> 收集会静默漏文件。**你的开源仓库是另一个、独立的仓库。**

### 步骤

```bash
# 1) 复制到 rt-thread 之外的独立目录（sibling，避免嵌套 git）
cp -r ~/gd32/rt-thread/open-source ~/gd32/gd32h759-rtthread-industrial

# 2) ★ 进入新仓库目录（关键：后面所有 git 操作都在这里）
cd ~/gd32/gd32h759-rtthread-industrial

# 3) 初始化并首次提交（这才是你的开源仓库）
git init -b main
git add -A
git commit -m "init: GD32H759 + RT-Thread 工控实战（设备 + 上位机 + 闭环）"

# 4) 关联远端并推送
git remote add origin git@github.com:<you>/gd32h759-rtthread-industrial.git
git push -u origin main
```

### 核对

```bash
pwd              # 应在 ~/gd32/gd32h759-rtthread-industrial
git remote -v    # 指向你的仓库（不是 RT-Thread）
git status       # 应干净（rt-thread/ 被 .gitignore 忽略）
git ls-files | wc -l
```

### 让 rt-thread 那边保持干净

```bash
# 仅本机忽略暂存目录，不改上游 .gitignore
echo 'open-source/' >> ~/gd32/rt-thread/.git/info/exclude
```

### 剩余手动项

- [ ] 填 `NOTICE` 署名（`<your name>`）；想换 MIT 就替换 `LICENSE`
- [ ] 补 README 顶部演示动图与正文截图（见 `BLOG_OUTLINE_10.md` 素材清单）
- [ ] （建议）联网跑一次 `./scripts/bootstrap.sh` 端到端验证
- [ ] 打 Release：`rtthread.bin` + `points.json` + `host` 安装包 + 演示视频 + `CHANGELOG.md`
