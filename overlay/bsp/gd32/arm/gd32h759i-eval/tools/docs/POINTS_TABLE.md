# 设备点表（全量）

> 设备：**GD32H759I-EVAL** · 共 **72** 点（当前编入 **69**）

> 来源：`applications/mb_points.c` → `tools/points.json` → 本表（由 `tools/gen_points_md.py` 生成）


## 线圈（0x，FC 0x01 / 0x05 / 0x0F）

| 点名 | 地址 | 参考号 | 类型 | 读写 | 绑定 | 启用 |
|---|---|:---:|---|---|---|---|
| `LED1` | `0x0000` | 1 | bit | rw | hook | ✓ |
| `LED2` | `0x0001` | 2 | bit | rw | hook | ✓ |
| `LOG_ENABLE` | `0x0700` | 1793 | bit | rw | hook | ✓ |
| `LOOP_ENABLE` | `0x0D00` | 3329 | bit | rw | hook | ✓ |


## 离散输入（1x，FC 0x02）

| 点名 | 地址 | 参考号 | 类型 | 读写 | 绑定 | 启用 |
|---|---|:---:|---|---|---|---|
| `KEY_WAKEUP` | `0x0000` | 10001 | bit | r | hook | ✓ |
| `KEY_TAMPER` | `0x0001` | 10002 | bit | r | hook | ✓ |
| `KEY_USER` | `0x0002` | 10003 | bit | r | hook | ✓ |
| `SD_MOUNTED` | `0x0700` | 11793 | bit | r | hook | ✓ |
| `TOUCH_PRESSED` | `0x0900` | 12305 | bit | r | hook | — |
| `USB_CDC_READY` | `0x0B00` | 12817 | bit | r | hook | ✓ |
| `ENET_LINK_UP` | `0x0C00` | 13073 | bit | r | hook | ✓ |


## 输入寄存器（3x，FC 0x04）

| 点名 | 地址 | 参考号 | 类型 | 读写 | 绑定 | 启用 |
|---|---|:---:|---|---|---|---|
| `SYS_MODEL` | `0x0000` | 30001 | u16 | r | reg | ✓ |
| `SYS_FW_VER` | `0x0001` | 30002 | u16 | r | reg | ✓ |
| `SYS_UPTIME_S` | `0x0002` | 30003 | u32 | r | hook | ✓ |
| `ADC_CH0` | `0x0200` | 30513 | u16 | r | hook | ✓ |
| `ADC_CH1` | `0x0201` | 30514 | u16 | r | hook | ✓ |
| `ADC_CH2` | `0x0202` | 30515 | u16 | r | hook | ✓ |
| `ADC_CH3` | `0x0203` | 30516 | u16 | r | hook | ✓ |
| `DAC_CH1_VAL` | `0x0300` | 30769 | u16 | r | hook | ✓ |
| `RTC_YEAR` | `0x0400` | 31025 | u16 | r | hook | ✓ |
| `RTC_MON` | `0x0401` | 31026 | u16 | r | hook | ✓ |
| `RTC_DAY` | `0x0402` | 31027 | u16 | r | hook | ✓ |
| `RTC_HOUR` | `0x0403` | 31028 | u16 | r | hook | ✓ |
| `RTC_MIN` | `0x0404` | 31029 | u16 | r | hook | ✓ |
| `RTC_SEC` | `0x0405` | 31030 | u16 | r | hook | ✓ |
| `EEP_RDATA` | `0x0500` | 31281 | u16 | r | hook | ✓ |
| `FLASH_MAGIC_OK` | `0x0600` | 31537 | u16 | r | hook | ✓ |
| `LOG_LINES` | `0x0700` | 31793 | u32 | r | hook | ✓ |
| `CAN_RX_CNT` | `0x0800` | 32049 | u32 | r | hook | ✓ |
| `CAN_LAST_ID` | `0x0802` | 32051 | u16 | r | hook | ✓ |
| `CAN_LAST_DLC` | `0x0803` | 32052 | u16 | r | hook | ✓ |
| `TOUCH_X` | `0x0900` | 32305 | u16 | r | hook | — |
| `TOUCH_Y` | `0x0901` | 32306 | u16 | r | hook | — |
| `MIC_LEFT` | `0x0A00` | 32561 | u16 | r | hook | ✓ |
| `MIC_RIGHT` | `0x0A01` | 32562 | u16 | r | hook | ✓ |
| `ENET_IP` | `0x0C00` | 33073 | u32 | r | hook | ✓ |
| `LOOP_EN` | `0x0D00` | 33329 | u16 | r | hook | ✓ |
| `LOOP_SP` | `0x0D01` | 33330 | u16 | r | hook | ✓ |
| `LOOP_FEEDBACK` | `0x0D02` | 33331 | u16 | r | hook | ✓ |
| `LOOP_OUTPUT` | `0x0D03` | 33332 | u16 | r | hook | ✓ |
| `LOOP_ERROR` | `0x0D04` | 33333 | s16 | r | hook | ✓ |
| `LOOP_ALARM` | `0x0D05` | 33334 | u16 | r | hook | ✓ |
| `LOOP_SCAN_US` | `0x0D06` | 33335 | u16 | r | hook | ✓ |
| `LOOP_JITTER_US` | `0x0D07` | 33336 | u16 | r | hook | ✓ |
| `LOOP_ITERS` | `0x0D08` | 33337 | u32 | r | hook | ✓ |


## 保持寄存器（4x，FC 0x03 / 0x06 / 0x10）

| 点名 | 地址 | 参考号 | 类型 | 读写 | 绑定 | 启用 |
|---|---|:---:|---|---|---|---|
| `DAC_CH1_SET` | `0x0300` | 40769 | u16 | rw | hook | ✓ |
| `RTC_YEAR_SET` | `0x0400` | 41025 | u16 | rw | hook | ✓ |
| `RTC_MON_SET` | `0x0401` | 41026 | u16 | rw | hook | ✓ |
| `RTC_DAY_SET` | `0x0402` | 41027 | u16 | rw | hook | ✓ |
| `RTC_HOUR_SET` | `0x0403` | 41028 | u16 | rw | hook | ✓ |
| `RTC_MIN_SET` | `0x0404` | 41029 | u16 | rw | hook | ✓ |
| `RTC_SEC_SET` | `0x0405` | 41030 | u16 | rw | hook | ✓ |
| `EEP_ADDR` | `0x0500` | 41281 | u16 | rw | hook | ✓ |
| `EEP_WDATA` | `0x0501` | 41282 | u16 | w | hook | ✓ |
| `FLASH_PARAM` | `0x0600` | 41537 | u32 | rw | hook | ✓ |
| `LOG_PERIOD_MS` | `0x0700` | 41793 | u16 | rw | hook | ✓ |
| `CAN_TX_ID` | `0x0800` | 42049 | u16 | rw | hook | ✓ |
| `CAN_TX_DLC` | `0x0801` | 42050 | u16 | rw | hook | ✓ |
| `CAN_TX_DATA0` | `0x0802` | 42051 | u16 | rw | hook | ✓ |
| `CAN_TX_DATA1` | `0x0803` | 42052 | u16 | rw | hook | ✓ |
| `CAN_TX_DATA2` | `0x0804` | 42053 | u16 | rw | hook | ✓ |
| `CAN_TX_DATA3` | `0x0805` | 42054 | u16 | rw | hook | ✓ |
| `CAN_TX_TRIG` | `0x0806` | 42055 | u16 | w | hook | ✓ |
| `LOOP_SETPOINT` | `0x0D00` | 43329 | u16 | rw | hook | ✓ |
| `LOOP_KP` | `0x0D01` | 43330 | u16 | rw | hook | ✓ |
| `LOOP_KI` | `0x0D02` | 43331 | u16 | rw | hook | ✓ |
| `LOOP_KD` | `0x0D03` | 43332 | u16 | rw | hook | ✓ |
| `LOOP_PERIOD_MS` | `0x0D04` | 43333 | u16 | rw | hook | ✓ |
| `LOOP_SAFE_OUT` | `0x0D05` | 43334 | u16 | rw | hook | ✓ |
| `LOOP_FB_LO` | `0x0D06` | 43335 | u16 | rw | hook | ✓ |
| `LOOP_FB_HI` | `0x0D07` | 43336 | u16 | rw | hook | ✓ |
| `LOOP_RESET` | `0x0D08` | 43337 | u16 | w | hook | ✓ |


---

> 说明：**参考号**为 Modbus 常用表示（0x→00001、1x→10001、3x→30001、4x→40001 起）；上位机实际使用**协议 0 基地址**（即 `地址` 列的 `0x....`）。

> 类型：`bit` 位 / `u16` / `s16` 有符号 / `u32` 占连续 2 寄存器（高字在前）；`绑定`为 `reg`（变量）或 `hook`（回调）。
