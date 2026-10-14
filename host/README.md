# GD32H759 SCADA — PC 上位机

《GD32H759 + RT-Thread 工控实战》第10篇的上位机：**Tauri v2 + Svelte**，
通过 **Modbus TCP** 监视/控制 GD32H759I-EVAL（从站）。

## 架构

```
[Svelte 前端]  ──invoke──>  [Rust 后端 src-tauri]  ──Modbus TCP──>  [GD32H759 从站]
   界面/轮询                    自包含 Modbus 客户端                  mb_pointtable
```

- 后端 `src-tauri/src/modbus.rs`：自包含 Modbus TCP 客户端（只依赖 tokio），支持 FC `01/02/03/04/05/06`，
  读响应**严格校验长度/字节数**（防错位、防越界）。
- 前端 `src/App.svelte`：读取**点表 `points.json`**，自动按区生成控件，并按连续地址合并读取区间。
- **区名契约**：后端 `src-tauri/src/main.rs` 的 `AREAS` 表用**规范区名**
  （`coil` / `discrete_input` / `input_register` / `holding_register`，即 `points.json` 的 `areas` 键），
  并保留短名（`din`/`ireg`/`hreg`）作兼容别名；改点表区名时两处一起改，`cargo test` 有单测锁住。
- 点表是**单一事实来源**：设备侧 `tools/gen_points_json.py` 生成 → 仓库根 `points.json` → `sync-points` 同步进前端。

## 依赖

- Node.js ≥ 18、npm
- Rust（stable）
- Tauri v2 系统依赖（Windows 需 WebView2；Linux 需 webkit2gtk 等，见 Tauri 官方文档）

## 使用

```bash
# 1) 安装依赖
npm install

# 2) 同步最新点表（设备侧重新生成 points.json 后执行一次）
npm run sync-points

# 3) 开发运行（自动拉起 Vite + Tauri 窗口）
npm run tauri dev
```

界面顶部填入设备 IP/端口（默认 502），点「连接」即可看到各子系统面板；
可写点（`attr` 含 `w`）会显示「写 / 翻转」控件。

### 点表从哪来

`scripts/sync-points.mjs` 按相对路径 `../../points.json` 解析，两种布局都成立：

| 布局 | `host` 位置 | 点表位置 |
|---|---|---|
| 开发树 | `<bsp>/tools/host` | `<bsp>/tools/points.json` |
| 开源仓库 | `<仓库根>/host` | `<仓库根>/points.json` |

## 功能

- **连接/轮询**：连接 Modbus TCP 从站，按连续地址合并区间轮询（默认 500ms），标题栏显示 `已读 X/Y 区间`。
- **实时趋势**：勾选任意数值点叠加到 canvas 曲线（无第三方图表库），保留最近 1200 个样本，
  支持**各序列独立缩放**（不同量程也能同屏）。
- **限值告警**：为任意数值点设置低/高限（`localStorage` 持久化），越限时顶部横幅 + 行高亮。
- **闭环联锁**：设备 `LOOP_ALARM` 置位时弹出联锁横幅。
- **CSV 导出**：把已缓存历史按长表 `time,name,value` 导出。
- **自动界面**：由点表 `points.json` 生成，写控件仅在 `attr` 含 `w` 且 `enabled` 时出现。

## 目录

```
host/
  index.html            Vite 入口
  package.json          npm 脚本与依赖
  scripts/sync-points.mjs   点表同步（仓库根 points.json -> src/lib/points.json）
  src/
    main.js             Svelte 挂载
    App.svelte          主界面（连接 + 趋势 + 告警 + 点表）
    app.css
    lib/api.js         Tauri 命令封装
    lib/history.js     历史缓存 + CSV 导出
    lib/Trend.svelte   canvas 趋势图
    lib/points.json    同步进来的点表（gitignore）
  src-tauri/
    Cargo.toml
    build.rs
    tauri.conf.json
    generate-ico.mjs   生成占位图标（node generate-ico.mjs）
    icons/icon.ico     Windows 资源所需图标
    src/main.rs        Tauri 命令
    src/modbus.rs      Modbus TCP 客户端
```

## 联调

上板验收逐步清单见 [`../docs/COMMISSIONING.md`](../docs/COMMISSIONING.md)
（开源仓库为仓库根 `docs/`；开发树为 `<bsp>/tools/docs/`）。

## 备注

- `tauri.conf.json` 中 `bundle.active=false`，只做 `dev` 运行；打包时需补图标并置 `true`。
- 触摸相关点在设备侧编译期关闭时 `enabled=false`，界面自动隐藏。
