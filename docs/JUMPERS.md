# GD32H759I-EVAL 跳线表（匹配当前工程配置）

> 依据：**GD32H759I-EVAL 使用指南 Rev2.2**（§4.1–4.22 原理图 + 官方例程章节的跳线说明）。
> 适用配置：`BSP_USING_ADC/DAC/RTC/I2C1/CAN1/CAN2/ETH/OSPI/SDIO/SDRAM/HPDF/USB_CDC` 已开；
> **TLI / TOUCH / ST7789 / DCI / I2S / SAI / SPI_LCD 未开**。
> 跳线帽位置以板上**丝印**为准；下表括号内是官方给出的 `Short JPx(a,b) for ...` 定义。

---

## 一、结论速查表（照这个插即可）

### 3 针选择跳线（1-2 / 2-3，2 为公共脚）

| 跳线 | 1-2 | 2-3 | **本工程** | 用途/连接 |
|---|---|---|---|---|
| JP40 | SDRAM | I2S | **1-2** | SDRAM（工程已开 BSP_USING_SDRAM） |
| JP41 | TLI | SPI_LCD | **2-3** | 未用（非 TLI 侧） |
| JP42 | TLI | KEY | **2-3** | User 键 `KEY3 = PF8` |
| JP43 | TLI | SAI | **2-3** | 未用 |
| JP44 | TLI | SAI | **2-3** | 未用 |
| JP45 | TLI | SPI_LCD | **2-3** | 未用 |
| JP46 | TLI | SPI_LCD | **2-3** | 未用 |
| JP47 | TLI | DAC | **2-3** | `DAC0_OUT1 = PA5` |
| JP48 | TLI | ETH | **2-3** | ENET |
| JP49 | TLI | SAI | **2-3** | 未用 |
| JP50 | TLI | LED | **2-3** | `LED1 = PF10` |
| JP51 | TLI | ETH | **2-3** | ENET |
| JP52 | TLI | SDIO | **2-3** | `SDIO0_D3 = PC11` |
| JP53 | TLI | CAN | **2-3** | `CAN1_RX = PB5` |
| JP54 | TLI | I2S | **2-3** | 未用 |
| JP55 | TLI | HPDF | **2-3** | `HPDF_DATA = PB1` |
| JP56 | TLI | HPDF | **2-3** | `HPDF_CKOUT = PB0` |
| JP57 | TLI | ETH | **2-3** | ENET |
| JP58 | TLI | I2S | **2-3** | 未用 |
| JP59 | TLI | ETH | **2-3** | ENET |
| JP65 | CAN (PB13=CAN1_TX) | SDIO (PB13=SDIO0_D0) | **见 ⚠ 冲突** | PB13 二选一 |
| JP66 | USB | LED | **2-3** | `LED2 = PA6` |
| JP67 | TLI/SPI_LCD | CAN | **2-3** | `CAN2_RX = PF6` |
| JP68 | USB | USART | **2-3** | `USART0_TX = PA9`（控制台！） |
| JP70 | USB | ETH | **2-3** | `ETH0_RMII_TXD0 = PB12` |

> 说明：JP41/43/44/45/46/49/54/58 的两侧都是**本工程未使用**的外设（TLI/SPI_LCD/SAI/I2S），
> 置 2-3（非 TLI 侧）即可，位置对本工程无功能影响。

### DCI 相关选择跳线（官方例程定义，丝印为 DCI ↔ 其它）

| 跳线 | 跳到 DCI 时 | 其它功能 | **本工程** |
|---|---|---|---|
| JP60 | DCI | ETH | **ETH**（ENET 例程要求匹配） |
| JP61 | DCI | OSPI | **OSPI**（OSPI 例程：`JP61 跳线到 OSPI`） |
| JP62 | DCI | SDIO | **SDIO**（SDIO 例程：`JP52/JP62/JP65 跳到 SDIO`） |
| JP63 | DCI | I2S | **I2S 侧**（本工程未用；DP 例程里 JP63 归 I2S） |
| JP34 | — | SAI | **非 SAI 侧**（SAI 例程才需要，本工程未用） |
| JP69 | — | USART | **USART**（SDRAM/OSPI 例程：`JP68/JP69 跳到 USART`） |

> JP34/JP60–63/JP69 官方 PDF 的**原理图文字层未抽出引脚号**，仅例程说明给出功能侧；
> 请对照板上丝印确认朝向，原则是"跳到上面写的功能侧"。

### 2 针跳线 / 接插件

| 跳线 | 类型 | **本工程设置** | 说明 |
|---|---|---|---|
| JP2 | 3 针（BOOT） | **短接到 GND 侧** | BOOT0=0，从内部 Flash 启动（正常跑程序） |
| JP12 | 2 针 | 按总线需要 | CAN1 的 **120Ω 端接**（R19）；作总线末端时短接 |
| JP14 | 2 针 | 按总线需要 | CAN2 的 **120Ω 端接**（R20） |
| JP16 | 2 针 | **短接**（默认） | `+3V3 ↔ VDD_MCU` 供电 |
| JP17 | 2 针 | **短接**（默认） | `+3V3 ↔ VCC_GDLINK` GD-Link 供电选择 |
| JP71 | 2 针 | 自检时**短接** | DAC/ADC 联合自检用（配合杜邦线 PA5↔PC2_C） |
| JP15 | TF 卡座 | 插卡 | SD 卡（SDIO） |
| JP4 | 2 针 | 默认 | 以太网 RJ45（HR911105A）相关 |
| JP30 | 20×2 排针 | 不插（可选） | LCD 模组座（TLI，未用） |
| JP13 | 接插件 | 不插 | SPI_LCD 模组座（未用） |
| JP7/8/9/10 | 22×2 排针 | 不插 | **扩展 I/O 排针**（非配置跳线） |
| JP100 | 4×1 | 默认 | 调试口 `L_SWDIO/L_SWDCK`（SWD） |

---

## 二、⚠ 硬件冲突：CAN1 与 SD 卡共用 PB13

- `PB13` 同时是 **CAN1_TX** 和 **SDIO0_D0**，由 **JP65** 二选一。
- 因此 **CAN1 与 SD 卡不能同时工作**。

选择：

| 目标 | JP65 | 结果 |
|---|---|---|
| 要 **SD 卡**（记录/文件系统） | **2-3 (SDIO)** | 占用 PB13 为 SDIO，**不要用 CAN1**；CAN 改用 **CAN2** |
| 要 **CAN1 收发** | **1-2 (CAN)** | SDIO 不可用，SD/记录点位会失败 |

**推荐：SDIO + CAN2**（工程已默认如此）
- `JP52 = 2-3`、`JP62 = SDIO`、`JP65 = 2-3`、`JP67 = 2-3`
- **CAN2 引脚不占 PB13**：`CAN2_TX = PF7`（无跳线）、`CAN2_RX = PF6`（JP67=2-3）
- 工程侧 CAN 服务已默认走 `can2`（`applications/app_can.c` 的 `CAN_SVC_DEV = "can2"`）。

> 若要改用 CAN1：`JP65 = 1-2`，并在 `app_can.c` 定义 `CAN_SVC_DEV="can1"`，此时放弃 SDIO。

---

## 三、各外设相关跳线（本工程用到的）

- **LED**：`JP50 → LED`(LED1/PF10)、`JP66 → LED`(LED2/PA6)
- **按键**：`JP42 → KEY`(User=PF8)；Wakeup=PA0、Tamper=PC13 无需跳线
- **ADC**：板载电位器 VR1 → `ADC2_IN0 = PC2_C`（无需跳线，直接旋电位器）
- **DAC**：`JP47 → DAC`(PA5)；示波器/万用表测 PA5；自检时短接 JP71 并接 PA5↔PC2_C
- **闭环控制(M6)**：`JP47 → DAC` + **杜邦线 PA5 ↔ PC2_C**（板载电位器 VR1 即负载/扰动）
- **EEPROM**：板载 AT24C02 在 I2C1（`SCL=PH4, SDA=PB11`），无需跳线
- **OSPI Flash**：板载 GD25X512ME，`JP61 → OSPI`
- **SDIO/TF**：`JP52=2-3`、`JP62 → SDIO`、`JP65=2-3`（见冲突节）
- **ENET(RMII)**：`JP48/JP51/JP57/JP59/JP60 → ETH`、`JP70 → ETH`
- **HPDF 麦**：`JP55/JP56 → HPDF`
- **CAN2**：`JP67 → CAN`；`JP53` 属 CAN1_RX(可 2-3 但不用 CAN1 时无影响)
- **USB CDC**：本工程用 **USBHS1 = CN3**（`drv_usbd_cdc.c` 的 `USB_CDC_USE_HS1=1`）→ **插 CN3**；
  `JP66/JP68/JP70` 的 USB 侧对应 USBHS0/CN2，本工程不用
- **控制台 USART0**：`JP68 → USART`（必需，否则无串口输出）

---

## 四、来源索引（官方指南 Rev2.2）

| 内容 | 章节 |
|---|---|
| 供电 / 启动 / LED / 按键 | §4.1–4.4 |
| ADC / DAC | §4.5 / §4.6（PA5=DAC0_OUT1，PC2_C=ADC2_IN0） |
| CAN（PB13/PB5/PF6/PF7，JP12/JP14） | §4.7 |
| ENET | §4.9 |
| HPDF | §4.10（JP55/JP56） |
| I2C（AT24C02） | §4.11 |
| OSPI | §4.14（JP61） |
| SDIO（JP15/JP65） | §4.16 |
| SDRAM（JP40） | §4.17 |
| TLI_LCD（JP41–JP59、JP67） | §4.18 |
| USART（JP68/JP69） | §4.19 |
| USB（JP66/JP68/JP70） | §4.20 |
| 扩展 / GD-Link / MCU（JP7–10/JP100） | §4.21–4.23 |
| 各例程跳线要求（OSPI/SDIO/DCI/ENET/SDRAM/SAI/I2S） | §5.x 对应 DEMO |
