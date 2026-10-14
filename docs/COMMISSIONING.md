# 第10篇 · M5 上板联调清单

> 目标：从上电到上位机全功能验收。`[ ]` 为勾选项，逐条通过即视为第10篇交付完成。
> 点表定义见 `applications/mb_points.c`；点表 JSON 见 `tools/points.json`（含精确参考号与启用状态）。

---

## 0. 前置检查

- [ ] 设备侧已按本文档所在工程的 `rtconfig` 配置编译（`scons -j8`），无 error。
- [ ] `tools/points.json` 已重新生成：`python3 tools/gen_points_json.py`，并同步到上位机 `npm run sync-points`。
- [ ] 硬件接线：
  - [ ] **跳线帽按 [`JUMPERS.md`](./JUMPERS.md) 设置（必须！）**
  - [ ] USART0 ↔ PC（115200-8-1-N），用于 msh 控制台
  - [ ] 网口 ↔ PC/交换机（ENET）
  - [ ] 若测 CAN：注意 `JP65` 使 **CAN1 与 SD 卡互斥**；建议用 CAN2（见 JUMPERS ⚠）
  - [ ] 若测 DAC/ADC 链路：PA5 ↔ PC2_C，JP71 短接
  - [ ] TF 卡已插入（测 SD/记录用）

---

## 1. 设备侧启动

- [ ] 上电，串口看到 RT-Thread 启动日志与 `msh />`。
- [ ] 确认外设已注册：
  ```
  msh> list_device
  ```
  应能看到 `adc2`、`dac`、`rtc`、`can1`、`sd0`、`usb-acm0`（USB 插入后）等。
- [ ] 启动从站：
  ```
  msh> modbus_start 502
  modbus tcp server on port 502, 50 points
  ```
  > 期望 **50 points**（触摸 3 点因 `BSP_USING_TOUCH` 关闭未编入；若开启触摸则为 53）。
- [ ] 观察是否有 `[W/mb.points]` 警告（如 adc2 not found），有则先解决。

## 2. 网络连通

> 本工程**未启用 netdev**，所以 msh 里没有 `ifconfig`（你现在能用的 `ping` 来自 lwIP 自带的
> `RT_LWIP_USING_PING`）。用本项目新增的 `net_info` 查看 `e0` 的 IP/链路；想要回标准
> `ifconfig` 见文末「附录：启用 netdev」。

- [ ] 设备侧查 IP：
  ```
  msh> net_info
  e0: ip=0.0.0.0 gw=0.0.0.0 mask=0.0.0.0 link=up if-up
    IP=0.0.0.0: DHCP 未拿到租约，可用 net_set 配静态 IP
  ```
  工程默认开 **DHCP**：有 DHCP 服务器时会自动拿到地址；没有则停在 `0.0.0.0`。
- [ ] 若 `0.0.0.0`，配静态 IP（**按你 PC 所在网段**，例如 PC 是 `192.168.2.x`）：
  ```
  msh> net_set 192.168.2.30 255.255.255.0 192.168.2.1
  e0 set: ip=192.168.2.30 mask=255.255.255.0 gw=192.168.2.1
  ```
- [ ] PC 侧：`ping <设备IP>` 通。
- [ ] 端口通：`Test-NetConnection <设备IP> -Port 502`（Windows）/ `nc -vz <IP> 502`。

### 命令行快速点验（可选，无需上位机）

任选一种工具验证协议通路（地址用**协议 0 基地址**，即点表里的 `addr`）：

```python
# pip install pymodbus
from pymodbus.client import ModbusTcpClient
c = ModbusTcpClient("192.168.1.30", port=502)
print(c.read_input_registers(0x0000, 3))   # 型号/固件/运行秒数(高)
print(c.read_input_registers(0x0200, 4))   # ADC CH0-3
c.write_register(0x0300, 2048)             # DAC 设定
c.write_coil(0x0000, True)                 # LED1 亮
```
> pymodbus 3.x 若报参数名错误，加 `slave=1`（或 `device_id=1`）；从站不校验单元号。

---

## 3. 上位机启动

- [ ] `cd tools/host && npm install`
- [ ] `npm run sync-points`（确保点表最新）
- [ ] `npm run tauri dev`（需 Rust + Tauri 系统依赖）
- [ ] 界面顶部填 `IP` / `502` → 点「连接」，状态显示「已连接」。
- [ ] 各面板数值开始刷新（默认 500ms）。

---

## 4. 分模块验收（点表地址为协议 0 基）

| 模块 | 地址（区 0x/1x/3x/4x） | 操作 | 期望 |
|---|---|---|---|
| LED | 线圈 `0x0000`/`0x0001` | 勾/翻转 | 板上 LED1(PF10)/LED2(PA6) 翻转，回读一致 |
| 按键 | 离散 `0x0000`~`0x0002` | 按 Wakeup/Tamper/User | 对应位变 1（松开变 0） |
| 系统 | 输入 `3x0000`~`0x0003` | 读 | 型号 `0x0759`、固件 `0x0100`、运行秒数递增 |
| ADC | 输入 `0x0200`~`0x0203` | 旋电位器/加信号 | 数值随信号变化（0~4095） |
| DAC | 写保持 `0x0300`；读输入 `0x0300` | 写 0→4095 | 回读一致；若接了 ADC 链路则 ADC 联动 |
| RTC | 读 `3x0400`~`0x0405`；写 `4x0400`~`0x0405` | 写年月日时分秒 | 回读与写入一致，秒位递增 |
| EEPROM | 保持 `0x0500`=地址、`0x0501`=数据；读输入 `0x0500` | 写字节后读回 | 读回等于写入 |
| Flash | 保持 `0x0600`(U32)；输入 `0x0600` | 写参数后读回 | 值一致，`FLASH_MAGIC_OK`=1 |
| SD | 离散 `0x0700` | 插/拔卡后读 | 挂载成功为 1 |
| 记录 | 保持 `0x0700`=周期；线圈 `0x0700`=1；输入 `0x0700`(U32)=行数 | 使能后观察 | 行数增长；`/adc_log.csv` 生成 |
| CAN | 读 输入 `0x0800`(U32)/`0x0802`/`0x0803`；写 保持 `0x0800`~`0x0806` | 触发发送/对端发帧 | 计数增长、最近 ID/DLC 更新 |
| 音频 | 输入 `0x0A00`/`0x0A01` | 对麦说话 | 电平上升 |
| USB | 离散 `0x0B00` | 插/拔 USB | 插上后为 1（`usb-acm0` 存在） |
| ENET | 离散 `0x0C00`；输入 `0x0C00`(U32) | 插/拔网线 | Link 位翻转；IP 与 `ifconfig` 一致 |
| 触摸 | `0x0900`(按下)/`3x0900`/`0x0901` | — | **当前编译期关闭**，界面自动隐藏；开启 `BSP_USING_TOUCH` 后出现 |

---

## 5. 趋势与告警演示

- [ ] 给 `ADC_CH0` 一个缓慢变化信号（电位器），趋势面板曲线平滑跟随。
- [ ] 勾选多个点位叠加显示，颜色区分、最新值端点可见。
- [ ] 给 `ADC_CH0` 设低限/高限，越过阈值时：顶部红色横幅出现、对应表格行高亮。
- [ ] 点「导出 CSV」，用 Excel/Python 打开，核对 `time,name,value` 数据连续。
- [ ] 点「清空历史」，曲线归零重画。

---

## 6. 故障排查

| 现象 | 排查方向 |
|---|---|
| `modbus_start` 报 busy | 已启动过一次；先 `modbus_stop` 或复位 |
| 上位机「连接失败」 | IP/端口、防火墙、设备 `ifconfig`、网线 |
| 某面板全 `—` 或 ⚠ | 该外设未注册；看 `list_device` 与启动日志 `[W/mb.points]` |
| ADC 读数不变 | 通道未使能/信号未接；`adc2` 是否存在 |
| CAN 计数不涨 | 波特率/模式、跳线、对端是否发送；CAN 服务以正常模式占用 **can2**（JP67→CAN） |
| 记录行数不涨 | `/` 是否有可写文件系统（SD 或 littlefs）；`fopen` 是否成功 |
| 触摸点缺失 | 正常：`BSP_USING_TOUCH` 未开，点表 `enabled=false` |
| ENET/IP 读不到 | `net_info` 看 `e0` 是否 `up`、是否拿到 IP；netif 名应为 `e0` |

---

## 7. 博客素材（建议留存）

- [ ] 设备侧 `modbus_start` 串口截图（含 50 points）。
- [ ] 上位机总览截图（趋势 + 告警横幅）。
- [ ] 一段「PC 改 DAC → 设备端 ADC 跟随」的联调录屏。
- [ ] `points.json` 与界面自动生成的对应关系示意。
- [ ] `gen_points_json.py` 读取 `rtconfig.h` 标注 `enabled` 的说明。

---

## 8. 验收标准

- [ ] 设备侧 50 点全部可读（写点可写并回读一致）。
- [ ] 上位机连接、轮询、趋势、告警、导出功能正常。
- [ ] 无未处理异常（Modbus 异常码、`⚠`）。
- [ ] 关键交互（LED / DAC / RTC / CAN / 记录）演示通过。

---

## 9. 闭环控制回路（M6）

目标：验证 **设定值 → PID → DAC → 负载 → ADC → 反馈** 的闭环，以及越限联锁。

**接线**：`JP47 → DAC`（PA5=DAC0_OUT1）+ **杜邦线 PA5 ↔ PC2_C**；板载电位器 VR1 即"负载/扰动"（两端会被 1K 负载分压，正好逼真）。

- [ ] 「保持寄存器」面板写入：`LOOP_PERIOD_MS=10`、`LOOP_SETPOINT=2000`、`LOOP_KP=120`、`LOOP_KI=5`、`LOOP_KD=0`、`LOOP_FB_LO=30`、`LOOP_FB_HI=4050`
- [ ] 趋势默认已选 `LOOP_SP / LOOP_FEEDBACK / LOOP_OUTPUT`
- [ ] 写线圈 `LOOP_ENABLE=1`
- [ ] 观察 `LOOP_FEEDBACK` 收敛到 `LOOP_SETPOINT`；`LOOP_OUTPUT` 稳定在补偿值（因分压，输出 > 设定）；`LOOP_ERROR → 0`
- [ ] 改 `LOOP_SETPOINT` → 反馈跟随
- [ ] 拔线/旋电位器到极端 → 反馈越限 → `LOOP_ALARM=1`、输出切 `LOOP_SAFE_OUT`、界面弹联锁横幅
- [ ] 恢复接线 + 写 `LOOP_RESET=1`（或关再开 `LOOP_ENABLE`）→ 恢复控制
- [ ] 读 `LOOP_SCAN_US`（100Hz 下 ≈10000us）与 `LOOP_JITTER_US` 看实时性

> 说明：闭环使能时 DAC 由控制回路驱动；未使能时不碰 DAC，仍可用 `DAC_CH1_SET` 手动输出。

---

## 附录：启用 netdev（可选，恢复 `ifconfig` / `netstat`）

`ifconfig`、`netstat` 由 **netdev 组件**提供；本工程默认关闭（`.config` 中
`CONFIG_RT_USING_NETDEV is not set`），故只有 lwIP 自带的 `ping`。

需要标准 `ifconfig` 时：

1. `menuconfig` → `RT-Thread Components → Network → Network interface device (netdev)` 打开，
   并确认其中的 `Enable ping` 等子项。
2. 保存退出（重新生成 `rtconfig.h`）→ `scons -j8` 重新编译。
3. 之后 msh 里出现 `ifconfig` / `netstat`。

> 本项目 ENET 点表与 `net_info` 走 **lwIP netif**，不依赖 netdev，开不开都能用。
