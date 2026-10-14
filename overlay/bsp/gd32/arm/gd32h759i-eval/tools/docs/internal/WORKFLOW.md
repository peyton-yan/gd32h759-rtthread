# 工作流：从开发到开源（一页看懂）

> 记住三个位置，后面所有操作都在其中之一进行。

| 角色 | 路径（示例） | 说明 |
|---|---|---|
| **开发树** | `~/gd32/rt-thread` | RT-Thread clone + 你的改动；只在这里**改代码 / 编译 / 调试** |
| **开源暂存** | `~/gd32/rt-thread/open-source` | 本项目"增量"的收集区（脚本生成） |
| **开源仓库** | `~/gd32/gd32h759-rtthread-industrial` | 真正 push 到 GitHub 的独立仓库 |

> ⚠️ **永远不要在开发树 `~/gd32/rt-thread` 里 `git add/commit`**：
> `collect.sh` 靠"未跟踪/未暂存"来收集文件，一旦 add 就会**静默漏收集**。
> （想让它干净：`echo 'open-source/' >> ~/gd32/rt-thread/.git/info/exclude`）

---

## ① 日常开发（在开发树）

```bash
cd ~/gd32/rt-thread/bsp/gd32/arm/gd32h759i-eval
scons -j8                       # 编译固件
# 上板调试；上位机：
cd tools/host && npm run tauri dev
```

- 改点表后：`python3 tools/gen_points_json.py`、`python3 tools/gen_points_md.py`
- 加/改代码请遵守 `AGENTS.md` 的目录守则（驱动 → `gd32_drivers`，app → `applications`）
- **不要直接改 pkgs 拉下来的包**（见 §⑤）

## ② 一轮改完 → 刷新开源暂存

```bash
bash ~/gd32/rt-thread/open-source/scripts/collect.sh
```

它会自动完成：

| 内容 | 去向 |
|---|---|
| 上游原生文件的**改动** | `patches/upstream-v5.2.2.patch` |
| **新增**文件（applications/board/gd32_drivers…） | `overlay/`（按原路径） |
| `tools/host` | `host/`（顶层独立） |
| `tools/points.json` | `points.json`（仓库根） |
| `tools/docs` | `docs/`（**非破坏式**，手动放的图片会保留） |

## ③ 首次发布到 GitHub

```bash
# a) 复制到 rt-thread 之外的独立目录
cp -r ~/gd32/rt-thread/open-source ~/gd32/gd32h759-rtthread-industrial

# b) ★ 进入新仓库目录（关键：不然 git 会落在 rt-thread 上）
cd ~/gd32/gd32h759-rtthread-industrial

# c) 初始化 + 首次提交 + 推送
git init -b main
git add -A
git commit -m "init: GD32H759 + RT-Thread 工控实战（设备 + 上位机 + 闭环）"
git remote add origin git@github.com:<you>/gd32h759-rtthread-industrial.git
git push -u origin main
```

核对：`pwd` 在**新仓库**；`git remote -v` 指向你的仓库；`git status` 干净。

## ④ 后续提交（每次迭代）

```bash
# 1) 先在开发树跑一遍收集
bash ~/gd32/rt-thread/open-source/scripts/collect.sh

# 2) 到开源仓库提交
cd ~/gd32/gd32h759-rtthread-industrial
git add -A && git commit -m "feat: ..." && git push
```

## ⑤ pkgs 包的改动怎么办（别直接改包）

`pkgs --update` 拉下来的库（如 `libraries/gd32-arm-series-latest`）是**独立 git 仓库**，
改了它：本仓库 overlay/patch 收不到、还可能被 `pkgs` 覆盖。

**规则：搬进 BSP。** 例：`bsp/gd32/arm/gd32h759i-eval/board/hal_supplement/`
（源文件 + `SConscript`，对象落 `build/`；包保持原样）。

## ⑥ 别人怎么复现（一键）

```bash
git clone <repo> && cd <repo>
./scripts/bootstrap.sh          # clone RT-Thread v5.2.2 + 覆盖 overlay + 打 patch

cd rt-thread/bsp/gd32/arm/gd32h759i-eval
pkgs --update                   # 取 GD32 SDK / CMSIS（第三方）
scons -j8                       # 固件

cd ../../../../host 2>/dev/null || cd <repo>/host
npm install && npm run sync-points && npm run tauri dev     # 上位机
```

## ⑦ 速查：改动落点

> 口诀：**新增 → overlay，改上游 → patch，改包 → 搬进 BSP，第三方 → pkgs。**

| 改动类型 | 落在哪 |
|---|---|
| BSP 新增源码（应用/板级/工具/自研驱动） | `overlay/bsp/...` |
| 上游原生文件的改动 | `patches/upstream-v5.2.2.patch` |
| pkgs 包的改动 | `overlay/.../board/hal_supplement/`（包还原原样） |
| 上位机 | `host/` |
| 点表 | `points.json`（仓库根） |
| 第三方 SDK / CMSIS | 不入库（`pkgs --update`） |

---

## 一图流程

```
开发树 ~/gd32/rt-thread               开源暂存 open-source/              开源仓库（GitHub）
  改代码/编译/调试  ──collect.sh──►   overlay/ + patches/ + host/  ──cp + git──►  git push
        ▲                              + points.json + docs/
        └───────────────── 迭代回填 ────────────────┘
```
