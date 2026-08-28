# mc02 固件指南

> **文档类型**：现行规范（板级）
> **适用范围**：`firmware/mc02/`，DM-MC02 / CtrBoard-H7（STM32H723VGT6）
> **状态**：现行有效
> **相关文档**：[仓库根 AGENTS.md](../../AGENTS.md) · [本目录 README.md](README.md)（外设与低延迟设计） · [UART_RING_LOG.md](UART_RING_LOG.md)（UART 实录与实测） · [仓库根 README.md](../../README.md)（烧录流程）

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。深入的外设/时钟/低延迟设计见本目录 `README.md`，此处只列 agent 关键点。

## 摘要

mc02 是 Cortex-M7 @ 550 MHz 的高性能板，特点是 **CAN-FD 常驻 FD+BRS** 与 **热路径代码
放 `.itcm`**；USB 受封装限制只能跑 Full-Speed，**瓶颈在 USB 还是在 CAN 取决于开几条
总线**（分界线见下方「关键特性」）。改这块板之前必须知道两件事：外设配置回 CubeMX 改，以及**每次 CubeMX 重新 Generate 之后要手工
复原一批改动**（清单在 [README.md](README.md)）。

## 芯片与工具链
- MCU：**STM32H723VGT6**（DM-MC02 / CtrBoard-H7），Cortex-M7 @ 550 MHz，LQFP100。
- ISA/工具链：ARM，`cmake/gcc-arm-none-eabi.cmake`，需 `arm-none-eabi-gcc`。

## 构建
```bash
cmake --preset debug -S firmware/mc02
cmake --build firmware/mc02/build --target mc02_app mc02_bootloader
```
- preset：`debug` / `release`。target：`mc02_app`、`mc02_bootloader`。

### build 目录已存在时，`option()` 的默认值不生效 [实测 2026-08-12]

`cmake --preset debug` 在**已有** `firmware/mc02/build/` 上运行时**沿用旧 cache**，
`app/CMakeLists.txt` 里 `option(... ON)` 的默认值不会被应用。曾经因此整整一轮测试都跑在
一个自以为是 ON、实际是 OFF 的配置上，连带做出错误结论。

改开关必须显式传：

```bash
cmake -DLIBRMCS_APP_IMU_ENABLE=ON firmware/mc02/build && cmake --build firmware/mc02/build --target mc02_app
```

核对当前生效值：

```bash
grep LIBRMCS_APP firmware/mc02/build/CMakeCache.txt
arm-none-eabi-nm firmware/mc02/build/app/mc02_app.elf | grep -c bmi088   # IMU: OFF=3, ON=57
```

## 编译开关

全部定义在 `app/CMakeLists.txt`，默认值即下表。**后四个都占用 `DataId::kUart0`，互斥。**

| 开关 | 默认 | 作用 |
|---|---|---|
| `LIBRMCS_APP_IMU_ENABLE` | ON | BMI088 初始化与采样 |
| `LIBRMCS_APP_RS485_ENABLE` | OFF | 把 USART2 的 RS-485 口暴露为 UART0；需 `.ioc` 的 USART2 DMA 已 Generate |
| `LIBRMCS_APP_USB_RX_HIST` | OFF | 相邻两次 bulk OUT 完成的间隔直方图（DWT）在 kUart0 上输出 |
| `LIBRMCS_APP_LOOP_PROFILE` | OFF | 主循环分段耗时（DWT）在 kUart0 上以 ASCII 输出 |
| `LIBRMCS_APP_CAN_DIAG` | OFF | CAN 遥测记录在 kUart0 上输出 |

### DWC2 内部 DMA：测过更慢，开关已删除 [实测 2026-08-28]

**结论：小包包率降约 3%，大包吞吐一点没动，板上主循环省下的时间可以忽略。没有任何一档
测下来是赢的，所以 `LIBRMCS_APP_USB_DWC2_DMA` 开关连同 `tusb_config.h` 里的
`CFG_TUD_DWC2_DMA_ENABLE` / `CFG_DWC2_MEM_UNCACHED_REGIONS` 一并删掉了，slave 模式是
唯一支持的配置**（删除前后默认镜像 bit 级相同，已 `cmp` 验证）。下面这份记录留着，
是为了别人不用再走一遍——**要重新加回这个开关，先把下面的测量重做一遍。**

A/B 方式：同一块板，`release` 预设，交替 DFU 烧录两个镜像，每档 3 次 5 s，
工具 `host/examples/mc02_packet_rate.cpp`（下行泛洪，记录发给没接线的 UART1，
固件把多余的丢掉，测的就是 USB + 反序列化这条路）。主机未跑 `host-tuning.sh`
（governor 仍是 powersave），两臂条件相同。

| 档位（秒 / 载荷 / 每包记录数） | OFF | ON | 差 |
|---|---|---|---|
| `5 8 1`（一包一条 8 B 记录） | 26650 / 26604 / 26628，第二轮 26623 / 26631 / 26563 包/s | 25934 / 25904 / 25912，第二轮 25808 / 25818 / 25921 包/s | **-2.9%** |
| `5 64 4`（约 300 B 传输） | 884.5 KB/s | 882.9 KB/s | -0.2% |
| `5 256 8`（约 2 KB 传输） | 1010.0 KB/s | 1009.0 KB/s | 0（都撞在 Full-Speed 线速上） |

为什么省下的拷贝换不回吞吐（**省的是 CPU 周期，付的是墙钟延迟，卡住的是后者**）：

- **拷贝本来就不在瓶颈上。** `LIBRMCS_APP_LOOP_PROFILE=ON` 再测一轮，主循环
  avg 从 4782 降到 4737 cycles（550 MHz 下 -0.08 us/pass），`usb` 段 821 -> 805 cycles。
  可是泛洪时主循环跑到 114-125 kHz，而包只有 25 kHz——**每来一个包主循环空转四五圈**，
  省 45 cycles 换不到任何东西。**CPU 更闲了包率却更低**，这本身就说明瓶颈不在 CPU。
- **真正卡住的是"包到 -> 端点重新 arm"这段墙钟时间**，因为主机是背靠背发的。用
  `LIBRMCS_APP_USB_RX_HIST=ON` 量相邻两次 OUT 完成的间隔（板上 DWT，采样点在
  `tud_vendor_rx_cb`），两臂各约 12900 样本/500 ms：

  | 间隔档 | slave（OFF） | DMA（ON，按样本数归一） |
  |---|---|---|
  | <20 us（同一串背靠背） | 2081 | **1830** |
  | 20-30 us | 3739 | 3739 |
  | 34-38 us | 743 | 803 |
  | 55-70 us | 3298 | 3443 |
  | >=90 us（尾部） | 223 | 224 |

  **少掉的全在 <20 us 那一档，尾部一个没多。** 所以不是"偶尔漏掉一整个 125 us 微帧"，
  而是**每串背靠背里少接住一个包**：DWC2 内部 DMA 要先把包从 RX FIFO 经 AHB 搬进
  AXI SRAM 才拉 `XFRC`，比 slave 模式"包一进 RX FIFO 就 `RXFLVL`、CPU 立刻开搬"晚，
  重新 arm 还多写一个 `doepdma`。晚这一点，就赶不上主机约 17 us 后的下一个 OUT 令牌。
  `[实测 2026-08-28，间隔直方图]`
- 缓存维护不是原因：TinyUSB 的传输缓冲区（`_vendord_epbuf` 0x240009a0、`_dcd_usbbuf`、
  `_ctrl_epbuf`）全在 AXI SRAM，且已由 `CFG_DWC2_MEM_UNCACHED_REGIONS` 声明为非缓存
  （`tusb_config.h`），DMA 臂上没有额外的 clean/invalidate。
- **这 3% 不是相位假象。** 见下一节：包率对主循环周期极其敏感（±35%），只在一个工作点
  上比较不可信。在**两个**相差 35% 的工作点上各测三次，DMA 的差都是 -3.0%：

  | | ballast 0 | ballast 825 cycles（1.5 us） |
  |---|---|---|
  | slave | 26637 / 26766 / 26736 | 36108 / 36050 / 36075 |
  | DMA | 25934 / 25903 / 25933 | 34918 / 35093 / 34987 |
  | 差 | -3.0% | -3.0% |

当时 DMA 确实生效了，不是没开：`CFG_TUD_DWC2_DMA_ENABLE=1` 会让
`CFG_TUD_DWC2_SLAVE_ENABLE` 默认变成 0（`tusb_option.h`），FIFO 拷贝那条路径整个编不进去
——`dcd_int_handler` 从 0x990 字节缩到 0x788——而板子照常枚举、照常跑 1 MB/s，
说明走的就是 DMA 路径。

**和 5321 没有可比性**：`rmcs_board` 的 HPM5321 用的是 `dcd_hpm.c`（ChipIdea/EHCI 式的
QHD/QTD 描述符控制器），本来就只有 DMA 一种工作方式，根本没有"FIFO 拷贝模式"可省，
也没有对应的编译开关。这个开关是 Synopsys DWC2 独有的，只对 `c_board` / `mc02` 有意义。

**但 5321 那边量出来的斜率可以用来验算这 3%**：[rmcs_board/AGENTS.md](../rmcs_board/AGENTS.md)
「因果实验：`cycle = turnaround x 1.2~1.4 + 约 13.5 us`」实测**每包周期对 turnaround
的斜率是 1.2~1.4**。把这条套到 mc02：DMA 让 turnaround 长约 1 us，预期包率损失
1 x 1.3 / 37.6 us ≈ **3.5%**，实测 3.0%。两块板、两种控制器、独立测出来的两个数对得上，
"DMA 的代价落在 turnaround 上"这个解释因此不只是本板的自圆其说。`[推断，基于两项实测]`

### 下行包率是主循环周期的强非单调函数，摆幅 ±35% [实测 2026-08-28]

**结论：给主循环每圈加 1.5 us 纯忙等，小包下行包率从 26700 涨到 36100（+35%）；加 3 us
反而掉回 26000，加 6 us 掉到 23900。这不是"CPU 太忙"——加的是纯浪费的忙等。任何
mc02 的 USB 性能 A/B，如果只在一个工作点上测，都可能测的是相位而不是改动本身。**

`LIBRMCS_APP_LOOP_BALLAST_CYCLES=N` 扫描，`mc02_packet_rate 4 8 1`，各三次取中位，
IMU=ON、无诊断开关（DMA=OFF 臂）：

| ballast | 0 | 825（1.5 us） | 1650（3 us） | 2200（4 us） | 3300（6 us） | 4400（8 us） |
|---|---|---|---|---|---|---|
| 包/s | 26700 | **36100** | 26100 | 25500 | 23900 | 27300 |

为什么会这样：整条链路是**闭环**——板子 arm 端点、主机才发下一个包、包到了板子再
arm。所以"包到 -> 重新 arm"的相位是自锁的，而 arm 只发生在 `tud_task()` 里，
即每个主循环周期一次。主循环周期与主机的事务节奏（微帧 125 us，串内背靠背约 17 us）
形成拍频：**相位好时一串能接住两个包，相位差时只接住一个**。直方图看得很清楚——
1.5 us ballast 那一臂 `<20 us` 档从 2081 涨到 5167，尾部（>=90 us）完全没动。

**这不是可以直接拿来用的优化**，两条理由：

- **最优点跟着主循环的组成变**。把 IMU 关掉（每圈少一次 BMI088 SPI 服务），同样的
  1.5 us ballast 从 +35% 变成 -7%：IMU=OFF 时 ballast 0 是 29100、ballast 825 是 27100。
  也就是说甜点位置由整圈的时序决定，任何一处改动都会挪走它。
- 它只对"主机背靠背泛洪小包"这一种流量成立。控制回路那种 1 kHz 的往返上面没测过。

**要用的话必须重测**，而且要连着扫几个 ballast 值确认自己不在悬崖边上。测量工具是
`LIBRMCS_APP_USB_RX_HIST=ON`（间隔直方图）加 `LIBRMCS_APP_LOOP_BALLAST_CYCLES`（相位旋钮）。

**5321 上没有这个现象，别照搬。** [rmcs_board/AGENTS.md](../rmcs_board/AGENTS.md)
2026-08-07 做过同一类实验（在 `rx_cb` 里 re-arm 之前注入忙等），六个点**单调下降**、
没有任何甜点：0 -> 54.3k、600 cy -> 52.5k、1200 cy -> 47.1k、1800 cy -> 40.8k。
两处的注入点不完全相同（5321 注在 `rx_cb` 里，本板注在主循环开头，因而还改了
`tud_task()` 的相位），但结论方向是清楚的：**High-Speed 的 5321 每包周期由约 13.5 us
的主机侧地板 + 1.2~1.4 倍斜率决定，是单调的；本板这个甜点是 Full-Speed 下主循环周期与
主机事务节奏打拍出来的，属于 mc02 特有。**

本板这个峰**还没有被完整解释**。已知的线索是它跟上行有关：关掉 IMU（上行几乎没数据了）
ballast 0 反而涨到 29100，说明上行 IN 事务本身在跟下行抢 Full-Speed 总线；而加 ballast
会把上行攒成更少更大的包。但这解释不了全部 35%。`[实测有，机制未定论]`

## 目录结构
- `app/`、`bootloader/`：两套独立镜像。`app/src/app.cpp` 提供自己的 `main()`，直接驱动生成的 `*_Config()` / `MX_*_Init()`。
- `bsp/cubemx/`：CubeMX 生成产物。`bsp/linker/`：手维护链接脚本（如 `STM32H723VGTx_APP.ld`，含 `.itcm` 热路径段）。
- `bsp/`：`cmsis-device-h7`、`stm32h7xx-hal-driver` 等第三方，视为只读。

## 关键特性（改代码前须知）
- USB：OTG_HS 跑 **Full-Speed**（LQFP100 无 HS PHY 引出）。**"瓶颈是 USB 还是 CAN"没有
  统一答案，按跑满的总线条数分界**：上行每帧 15 B（`1 + 3 - 1 + 8 + 4`，见
  [HOST_TUNING.md](../../HOST_TUNING.md) 9.3），CAN-FD 每条总线上限 19870 帧/s，而本板
  USB 聚合上限约 800 KB/s。于是 **1-2 条总线跑满时卡在 CAN 线速**（298 / 596 KB/s），
  **3 条一起跑满时才卡在 USB**（894 KB/s > 800）。分界点约 2.7 条总线，即聚合 53000 帧/s。
  `[推断，基于 800 KB/s 与 19870 帧/s 两项实测]`
- CAN：FDCAN1/2/3 常驻 FD+BRS，逐帧按 host `is_fdcan` 切换，不做 INIT 重配。
- **下行 CAN 帧直写硬件 FIFO，只有 FIFO 满了才进队列** [2026-08-24 修复]。
  `handle_downlink` 由 `tud_vendor_rx_cb` 在 `tud_task()` 里调用，与 `try_transmit()`
  同线程，所以直写是安全的（队列非空时必须让路，否则会插队）。
  **改之前是无条件入队**，于是每一帧都要等到主循环末尾的 `canN->try_transmit()` 才
  进硬件，中间隔着 DFU poll、GPIO 采样、一次 BMI088 SPI 读和 LED poll；同时那个
  16 深的环（继承自 c_board，那块板 bxCAN 只有 3 个发送邮箱，16 是净赚）架在 32 条
  FDCAN FIFO **前面**，把单包突发上限从 32 砍到了 16。队列深度现在是 64
  （`kTransmitQueueSize`，每路 1 KB DTCM），与 rmcs_board 一致。
  **实测效果**（交替烧录 A/B，三轮各 4000 帧，已跑 `host-tuning.sh`；
  5321 -> mc02 方向作对照组，三轮 p50 131.2/131.1/131.1 -> 131.6/131.0/131.4，确认未动）：

  | mc02 -> 5321 | 改前（三轮） | 改后（三轮） |
  |---|---|---|
  | CAN-FD min | 94.6 / 95.0 / 94.7 us | **90.9 / 92.9 / 91.8 us** |
  | CAN-FD p50 | 124.8 / 124.7 / 124.7 us | **123.6 / 123.7 / 123.7 us** |
  | CAN-FD avg | 125.4 / 125.9 / 125.3 us | **121.7 / 122.8 / 122.5 us** |
  | classic p50 | 180.5 / 180.5 / 180.5 us | **179.7 / 179.8 / 179.5 us** |
  | classic p90 | 209.9 / 207.6 / 209.4 us | **206.7 / 206.7 / 206.2 us** |
  | 单包突发 17/24/32/40/64 帧 | 丢 5.9/33/50/60/75% | **全部 0%** |

  **改后 avg 落到 p50 之下**（122.3 vs 123.7；改前是正常右偏的 125.4 vs 124.7）：分布
  变成双峰，一部分帧真的走了直通路径（min 掉到 91 us），把均值拉到中位数以下。这也是
  为什么均值改善 3.2 us 而中位数只有 1.1 us。

  **p99 / max 没有可重现的改善**，且调优后 max 在**所有臂包括对照组**仍是 630-950 us。
  那是主机侧的：`mixed_board_test` 经 `multi_board.hpp` 构造 session，**没有传
  `thread_setup`，事件线程没绑核**，而这是 [HOST_TUNING.md](../../HOST_TUNING.md) 1.3
  记的尾部最差一档。**要评估板级抖动，得先给测量工具加上绑核能力**，否则测的是主机调度。
  `[实测 2026-08-24，mc02 <-> 5321，已调优主机、事件线程未绑核]`
- **未做**：rmcs_board 那套下行流控（`transmit_queue_depth()` 决定是否再 arm OUT 包）
  没有移植。所以队列真被打满时，mc02 仍然是静默丢弃 + 点 LED，主机无感——
  `diag::note_tx_fail()` 在默认构建下是空实现（`LIBRMCS_APP_CAN_DIAG` 默认 OFF）。
- 热路径 `Can::handle_uplink/handle_downlink/try_transmit` 等放 `.itcm`，启动时从 FLASH 拷入。
- UART：四个口各一条**永不停的整环 circular DMA**，写指针由主循环读 `NDTR` 推导，不由中断维护。端口对象（含 DMA 环）放 `.d2_sram`，启动时从 FLASH 拷入，MPU region 1 在 `app.cpp` 里设为非缓存。**不要给 UART 的 DMA 开 FIFO/burst**——`NDTR` 只统计到 DMA FIFO，写指针会算错。
- UART 错误策略：`CR3.OVRDIS=1`、**`CR3.DDRE=0`**、`CR3.EIE=0`。`DDRE` 是"出错时禁用 DMA"，**置 1 会让一个坏字符永久杀死端口**——细节见 [UART_RING_LOG.md](UART_RING_LOG.md) 第 1 章。
- 实测吞吐天花板约 **800 KB/s 聚合**（USB Full-Speed 决定），781 KB/s 时零丢失；主循环在满过载下仍有约 10 倍余量。

## 不要用 HAL_RCCEx_GetPeriphCLKFreq() 取 UART 内核时钟 [实测 2026-08-05]

**结论先行：本版 HAL 的 `HAL_RCCEx_GetPeriphCLKFreq()` 对两个 UART 组都返回 0。**
它的 if/else 链只覆盖 SAI / SPI / ADC / SDMMC / SPI6 / FDCAN，
`RCC_PERIPHCLK_USART16910` 和 `RCC_PERIPHCLK_USART234578` **一个分支都没有**，
直接掉到末尾 `else { frequency = 0; }`（`bsp/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rcc_ex.c`）。
那两个宏本身是存在的，所以编译期没有任何提示。

后果：`uart.hpp` 的 `handle_config()` 拿到 0 之后撞上自己的
`kernel_clock_hz == 0` 提前返回，**`BRR` 一次都没写过**，主机发来的运行时波特率
请求被静默忽略，端口永远停在 CubeMX 的 115200。

**正确做法**（已改成这样）：用 HAL 自己的 `UART_GETCLOCKSOURCE(handle, src)` 宏——
它按外设实例分派，正是 `UART_SetConfig()` 在 init 时算 `BRR` 用的同一个宏——
再按 `UART_CLOCKSOURCE_*` 取 `HAL_RCC_GetPCLK1Freq()` / `PCLK2` / HSI（**要按
`RCC_FLAG_HSIDIV` 右移**）/ CSI / LSE / PLL2Q / PLL3Q。不要自己手写"哪个口属于
哪个时钟组"的判断，那是在重复这个宏已经做对的事。

### 为什么 c_board 两行就够、这块板不行

c_board 同样的功能只用两行（`HAL_RCC_GetPCLKxFreq()`，见
`firmware/c_board/app/src/uart/uart.hpp` 的 `peripheral_clock_hz()`），**那在 F407
上是对的**：STM32F407 的 UART 直接挂在自己的 APB 总线上，总线频率就是内核时钟，
连 `Init.ClockPrescaler` 字段都不存在。STM32H7 在中间插了**每组可选时钟源 + 预分频器**，
所以必须先解析时钟源，频率才有意义。**别看着 c_board 简单就照抄过来。**

**为什么很难发现**：`UART7 <-> UART10` 自环测试在 115200 到 2000000 **全部 PASS**。
自环把两端同时设成目标值，配置没生效时两端**一起**留在 115200，于是波特率一致、
通信正常、测试通过。**自环只能验证两端一致，不能验证两端等于你要的值。**
要验证切换真的生效，必须**一端切、另一端不切**并确认通信变坏，或者跨板对打。
另见 [rmcs_board/AGENTS.md](../rmcs_board/AGENTS.md) 里同一次排查的 5321 侧 DLAB 坑。

## CubeMX 纪律（本板适用）
- 配置改在 CubeMX（`.ioc`），人工 Generate；**禁止**直接改 `bsp/cubemx/Core/` 生成代码。
- **每次 CubeMX “Generate Code” 后需手工复原**（会被覆盖），清单见 `README.md` 末节，例如删除 `Core/Src/main.c` 里重新生成的 `int main(void)`、把 `static void MPU_Config` 改回 `void MPU_Config`。
