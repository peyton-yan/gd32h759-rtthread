# 文档索引

## 随开源仓库发布（→ GitHub `docs/`）

| 文档 | 说明 |
|---|---|
| [POINTS_TABLE.md](POINTS_TABLE.md) | **点表全表**（72 点，按区列出；自动生成） |
| [JUMPERS.md](JUMPERS.md) | GD32H759I-EVAL **跳线表**（含 CAN1/SDIO 共用 PB13 的冲突） |
| [DMA_LAYOUT.md](DMA_LAYOUT.md) | **DMA / D-Cache / MPU 内存布局**（非cacheable 区、池分配器） |
| [COMMISSIONING.md](COMMISSIONING.md) | **上板联调清单**（含闭环 PID 第 9 节） |

## 仅本地使用（不发布，位于 `internal/`）

| 文档 | 说明 |
|---|---|
| [internal/OPEN_SOURCE.md](internal/OPEN_SOURCE.md) | 开源方案（overlay + patch、**改动落点总览**、发布步骤） |
| [internal/WORKFLOW.md](internal/WORKFLOW.md) | **工作流**：从开发到开源的一页速查 |
| [internal/BLOG_OUTLINE_10.md](internal/BLOG_OUTLINE_10.md) | 第10篇博客提纲（单篇 · 结果导向 · 图谱化） |

> `scripts/collect.sh` 只把"**随开源仓库发布**"的文档拷进发布目录 `docs/`；
> `internal/` 与本索引（README.md）都会被跳过。

## 生成链（单一事实来源）

```
applications/mb_points.c
   │  写点：MB_PT(区, 地址, 类型, 读写, reg/hook, "点名")
   ├─(tools/gen_points_json.py)─► tools/points.json ──┬─► 上位机 host/（自动生成界面）
   │                                                  └─(tools/gen_points_md.py)─► docs/POINTS_TABLE.md
   └─ 地址分块：0x00 系统 … 0x0D 闭环控制 … 0x0E 电机(预留)
```

## 常用命令

| 命令 | 位置 | 用途 |
|---|---|---|
| `python3 tools/gen_points_json.py` | 设备 BSP 目录 | 生成 `points.json` |
| `python3 tools/gen_points_md.py` | 设备 BSP 目录 | 生成 `POINTS_TABLE.md` |
| `npm run sync-points` | `host/` | 点表同步进前端 |
| `npm run tauri dev` | `host/` | 运行上位机 |
| `scons -j8` | 设备 BSP 目录 | 编译固件 |
