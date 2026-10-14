# DMA 内存布局规划（GD32H759I-EVAL / Cortex-M7）

> 面向《GD32H759 + RT-Thread 工控实战》第10篇的**内存与 DMA 布局**说明。
> 起因：Cortex-M7 带 D-Cache，DMA 与 CPU 访问同一块内存必须处理 cache 一致性；
> 本项目曾因"cache 维护按 32B cache line 对齐放大"踩坏相邻内存而崩溃
> （SDIO 驱动注释记录的 `pc=0 INVSTATE`）。本文给出统一、可复制的布局方案。

---

## 1. 为什么要专门规划

- **D-Cache 打开**（`RT_USING_CACHE=y`），cache line = 32 字节。
- DMA 走总线直接读写 RAM，**绕过 D-Cache**。于是：
  - CPU 写了缓冲还没回写内存 → DMA 读到旧数据；
  - DMA 写了内存 → CPU 从 cache 读到旧数据。
- 解决办法有两条：
  1. **逐次 cache 维护**（clean/invalidate）——必须按 32B 对齐**放大**处理，
     一旦缓冲不是 32B 对齐/整数倍，就会 invalidate 到相邻内存，**踩坏别人的变量**（本项目踩过）。
  2. **把 DMA 缓冲放进 D-Cache 管不着的内存**（MPU Non-cacheable）——CPU 与 DMA 天然一致，
     不再需要任何逐次维护。**本项目的标准做法选它。**

## 2. 物理内存分区（`board/linker_scripts/link.ld`）

| 区域 | 起始 | 大小 | 用途 | Cache 属性 |
|---|---|---|---|---|
| CODE | `0x08000000` | 3M | 内部 Flash：代码/只读 | — |
| DATA | `0x24000000` | 448K | AXI SRAM：.data/.bss/栈（CPU 专用） | 可 cache（默认） |
| **DMARAM** | `0x24070000` | **64K** | **`.dma_buffer`：所有 DMA 缓冲** | **MPU 非cacheable** |
| ITCMRAM | `0x00000000` | 64K | 紧耦合指令 RAM（预留） | 无 cache |
| DTCMRAM | `0x20000000` | 64K | 紧耦合数据 RAM（预留，DMA 不可达） | 无 cache |
| ENETRAM | `0x30000000` | 16K | ENET 描述符/收发缓冲 | MPU 非cacheable |
| SDRAM | `0xC0000000` | 32M | 外部 SDRAM（TLI/DCI 帧缓冲） | MPU 非cacheable |

> 说明：`DMARAM` 从 AXI SRAM 尾部切出 64K，与 `DATA` 不重叠；
> 之所以从 AXI SRAM 切而不是用 DTCM，是因为**多数 DMA 控制器访问不到 DTCM**。

## 3. MPU 区域分配（Cortex-M7 共 16 个）

| Region | 基址 | 大小 | 属性 | 所有者 |
|:---:|---|---|---|---|
| 0 | `0xC0000000` | 32M | Normal Non-cacheable Shareable, XN | `drv_sdram_h7.c`（扫描空闲区） |
| 1 | `0x30000000` | 16K | Normal Non-cacheable, bufferable | `drv_enet.c`（扫描空闲区） |
| **4** | **`0x24070000`** | **64K** | **Normal Non-cacheable Shareable, XN** | **`drv_dma_mem.c`（固定）** |
| 其余 | — | — | 空闲 | 预留 |

**规则**：
- 驱动用 MPU **必须扫描空闲 region**，不能硬编码——本项目曾因 `drv_enet` 硬编码 region 0
  覆盖了 `drv_sdram` 的 region 0，导致 SDRAM 的 non-cacheable 失效。
- `drv_dma_mem.c` 使用固定的 region 4（DMA 区地址固定，简单直接）。

## 4. 各外设 DMA 缓冲登记表

| 外设 | 缓冲 | 当前安置 | Cache 维护 |
|---|---|---|---|
| **SDIO** | `sdio_idma_bounce` (8160B) | ✅ `.dma_buffer`（静态段） | 不再需要 |
| **HPDF 麦** | `pcm_left/pcm_right` (8KB) | ✅ `.dma_buffer`（静态段） | 不再需要 |
| **USART DMA RX** | 框架 rx_fifo 缓冲 | ✅ `.dma_buffer`（池分配，见下） | 不再需要 |
| ENET | 描述符 + rx/tx buff | ✅ ENETRAM + MPU region | 不需要 |
| ADC/DAC | 测试用 DMA 缓冲 | ⬜ 现状 32B 对齐 + flush/invalidate | 按需 |
| OSPI / SPI | 块缓冲 | CPU 轮询为主 | 按需 |

> ✅ = 已按新方案落地；⬜ = 现方案可用，后续按同一规则迁移。

## 5. 落地实现

### 5.1 链接脚本（`link.ld`）
```ld
DMARAM (xrw) : ORIGIN = 0x24070000, LENGTH = 64K
...
.dma_buffer (NOLOAD) :
{
    . = ALIGN(32);
    _dma_buffer_start = .;
    *(.dma_buffer)
    *(.dma_buffer*)
    . = ALIGN(32);
    _dma_buffer_end = .;
} > DMARAM
```

### 5.2 MPU 配置（`drv_dma_mem.c`，`INIT_BOARD_EXPORT` 早期执行）
```c
ARM_MPU_SetRegion(ARM_MPU_RBAR(4, 0x24070000),
                  ARM_MPU_RASR(1, ARM_MPU_AP_FULL, /*TEX*/1, /*S*/1, /*C*/0, /*B*/0, 0,
                               ARM_MPU_REGION_SIZE_64KB));
```
开机日志：`[I/drv.dma] DMA non-cacheable: 0x24070000 +64KB (region 4), used 8160 B`

### 5.3 外设声明缓冲（静态，示例：SDIO / HPDF）
```c
static rt_uint8_t sdio_idma_bounce[8160]
    __attribute__((aligned(32)))
    __attribute__((section(".dma_buffer")));
```

### 5.4 动态 DMA 缓冲池（`drv_dma_mem.c`）
`.dma_buffer` 上实现了极简 first-fit 分配器（32B 对齐，支持 free/合并）：

```c
void *p = rt_dma_malloc(n);   /* 从非cacheable DMA 区取缓冲 */
rt_dma_free(p);
```

**串口框架接入**：`dev_serial.c` 提供可覆盖的弱钩子，
`drv_dma_mem.c` 给出强定义，于是 **DMA_RX 模式的串口接收 FIFO 自动进池**：

```c
/* dev_serial.c（弱，默认走堆） */
__attribute__((weak)) void *rt_serial_dma_buf_alloc(rt_size_t size) { return rt_malloc(size); }
__attribute__((weak)) void  rt_serial_dma_buf_free(void *ptr)       { rt_free(ptr); }

/* drv_dma_mem.c（强，改走非cacheable 池） */
void *rt_serial_dma_buf_alloc(rt_size_t size) { return rt_dma_malloc(size); }
void  rt_serial_dma_buf_free(void *ptr)       { rt_dma_free(ptr); }
```
> 这样串口 DMA 与 SDIO/HPDF 一样，**不再需要任何逐次 cache 维护**。

## 6. 新增 DMA 外设的检查清单

- [ ] 缓冲声明加 `__attribute__((section(".dma_buffer")))`（并对齐到 cache line：`aligned(32)`），
      或在运行时用 `rt_dma_malloc()` 分配。
- [ ] **不要再**在传输前后手工做 cache clean/invalidate（非cacheable 区无需）。
- [ ] 若历史代码已有 cache 维护，可以保留（对非cacheable 是空操作），但不要依赖它。
- [ ] 确认 `_dma_buffer_start.._dma_buffer_end` 不超过 64K（`drv_dma_mem.c` 打印池大小）。
- [ ] 多外设共享一块静态缓冲时，务必用锁串行化（本项目 SDIO 由 host 锁保证）。

## 7. 本次修复要点（对应"插 SD 卡开机死机"）

- 现象：插卡开机在 SDIO 50MHz 切换后 `INVSTATE`，`r03=0x2`（定时器回调被踩）；
  拔卡即正常。
- 定位：SysTick 里执行软件定时器回调时 PC 跳到 0x2 → **内存被破坏**，
  与 SDIO IDMA 的 cache 维护属同一类问题。
- 处理：
  1. SDIO bounce buffer 迁入 `.dma_buffer`（非cacheable）；
  2. `drv_enet` MPU 不再硬编码 region 0；
  3. 加大偏小的线程/中断栈：`_system_stack_size 0x200→0x1000`、
     `RT_MMCSD_STACK_SIZE 1024→2048`、`RT_SDIO_STACK_SIZE 512→1024`、
     `RT_TIMER_THREAD_STACK_SIZE 512→1024`。

## 8. 验证方法

- 构建后看 `rtthread.map`：
  ```
  .dma_buffer   0x0000000024070000   0x1fe0
  ```
- 串口开机日志应出现 `[I/drv.dma] DMA non-cacheable: 0x24070000 ...`。
- `list_device` 有 `sd0`；`net_info` 正常；插卡不再死机。
