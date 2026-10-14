# hal_supplement — GD32 标准外设库的 BSP 侧补充

## 为什么有这一层

`pkgs --update` 会把 GD32 标准外设库 clone 到
`bsp/gd32/arm/libraries/gd32-arm-series-latest`，那是一个**独立的 git 仓库**，
不在本工程 git 之下。直接改包内文件有两个问题：

1. 不受本工程 git 管理 → 本仓库的 overlay / patch **收不到**这份改动；
2. `pkgs --update` 可能把它覆盖掉。

因此把需要的外设源文件**搬到本目录**，随 BSP 一起受 git 管理，
并用 `SConscript` 把它们编进固件（对象落在 `build/board/hal_supplement/`）。

## 本目录内容

| 文件 | 来源 | 说明 |
|---|---|---|
| `gd32h7xx_dci.c` | 包内同版本 | 包内 `GD32H7xx/SConscript` 未含，DCI 驱动需要 |
| `gd32h7xx_ospi.c` / `gd32h7xx_ospim.c` | 包内同版本 | OSPI 驱动需要 |
| `gd32h7xx_timer.c` | 包内同版本 | ADC / DAC / HWTIMER 需要 |
| `gd32h7xx_trigsel.c` | 包内同版本 | ADC / DAC 需要 |
| `gd32h7xx_hpdf.c` | **GigaDevice V1.4.0** | 包内为 V1.2.0，此处升级（头文件 API 相同） |
| `migrated-from-pkg.patch` | — | 迁移前"包内被改"的完整 diff（存档） |
| `SConscript` | — | 按需编译上述源文件；`CPPPATH` 指向包内 Include |

- **头文件仍用包内的**（`GD32H7xx/GD32H7xx_standard_peripheral/Include`），故 `hpdf.c` 的 V1.4.0
  与 V1.2.0 头共存无碍（两版头文件 API 一致）。
- 包内 `GD32H7xx/SConscript` 已**还原为上游原样**。

## 维护提醒

- 升级 GD32 包后可对比包内版本：除 `hpdf` 外，其余尽量与包内保持同版本（用 `cp` 覆盖即可）。
- 新增其它外设源文件时，同样放这里并在 `SConscript` 里加一行（保持"增量都在 BSP"的原则）。
