# rmcs_board 固件指南

> **文档类型**：现行规范（板级）
> **适用范围**：`firmware/rmcs_board/`，HPMicro HPM6E8Y / HPM5321（Andes RISC-V）
> **状态**：现行有效
> **相关文档**：[仓库根 AGENTS.md](../../AGENTS.md) · [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md)（完整环境搭建） · [ecat/README.md](ecat/README.md)（EtherCAT 桥）

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。完整编译环境（依赖清单、工具链下载、烧录）见本目录 `BUILD_ENVIRONMENT.md`，此处只列 agent 关键点。

## 摘要

rmcs_board 与其他三块板最大的不同：它是 **RISC-V（Andes 核）**、用 **HPM SDK 超级构建**，
而且同一块板上有**两套互斥的固件镜像**（USB 数据固件 / EtherCAT 桥），刷一个会覆盖另一个。
工具链是仓库外的预编译二进制，装在哪由环境变量决定——本机若未安装属正常，见
[仓库根 AGENTS.md 开发机环境路径约定](../../AGENTS.md#开发机环境路径约定重要先读这条再看任何路径)。

## 芯片与工具链
- MCU：**HPM6E8Y / HPM5321**（HPMicro，**Andes RISC-V 核，不是 ARM**）；HPM6E8Y 双核。
- ISA/工具链：RISC-V，用 HPMicro GNU 工具链 `rv32imac_zicsr_zifencei_multilib_b_ext`（带 B 扩展 multilib），需 `riscv32-unknown-elf-gcc`。
- 工具链是仓库外预编译二进制，约定放 `~/3rd_party/hpm/`（`[前机路径]`，**不要**入库或做 submodule）。
- HPM SDK **v1.12.0 随仓库自带**（`bsp/hpm_sdk`，submodule），无需另装。submodule 指向
  fork `zyitom/hpm_sdk`（含 tinyusb vendor class / ZLP / full-speed 三处本地补丁），
  当前提交 `v1.12.0-3-ge4347411`，分支 `migrate-v1.12.0`。

## 三套独立镜像
| 固件 | 源码目录 / 开关 | 传输 | 说明 |
|---|---|---|---|
| USB 数据固件（单核） | `firmware/rmcs_board/`（超级构建，默认） | USB vendor bulk | **USB 延迟最低（p50 100us）**，core1 不释放 |
| EtherCAT 桥（旧布局） | `firmware/rmcs_board/ecat/` | EtherCAT（ARQ 流） | core0=SSC+USB / core1=CAN，迁移前的对照基线 |
| 核对调布局（新，主线） | `firmware/rmcs_board/` + `-DLIBRMCS_RELEASE_CORE1=ON`，core1 源码在 `ecat/core1_ecat/` | EtherCAT + USB，可切换 | core0=USB+CAN+协议栈 / core1=EtherCAT。烧录 `./flash-ecat-swap.sh`。见 [ecat/CORE_SWAP_MIGRATION.md](ecat/CORE_SWAP_MIGRATION.md) |

> 三者是独立镜像，**同一时刻只能刷其中一个**；刷任一套会覆盖另外两套。

> **选型要点（实测 2026-08-01，各 3 轮）**：**单核 USB 在每一个分位上都赢，包括尾部**，
> 只要一块桥板够用、USB 线长够用，它就是低延迟优先时的选择。EtherCAT 换来的不是速度，
> 而是**确定性**和多从站/长距离能力。完整分布、C-state 影响及原始数据见
> [ecat/DESIGN.md](ecat/DESIGN.md) 3.5 / 3.6 节；被推翻的假设见
> [CORE_SWAP_MIGRATION.md](CORE_SWAP_MIGRATION.md) 4.6 节。[实测]
<!--
原始分位数据已集中维护在 ecat/DESIGN.md 3.5 / 3.6 节。
> **单核 USB 在每一个分位上都赢，包括尾部，而且三轮高度一致。** 只要一块桥板够用、
> USB 线长够用，它就是最优解。EtherCAT 换来的不是速度，是**确定性**（p99-p50 只有 7us）
> 和多从站/长距离能力——固定延迟能在控制器里补偿，随机抖动不能。
>
> 核对调镜像的 USB 尾部**不可复现**（1/3 的轮次 max 跳到 250），这是它最需要注意的性质。
>
> 完整分布、C-state 的影响、以及两条被否掉的主机侧调优见
> [ecat/DESIGN.md](ecat/DESIGN.md) 3.5 / 3.6 节；被推翻的假设见
-->

## 跨芯片对比：HPM5321 DualCan vs HPM6E8Y [实测 2026-08-01，各 3 轮]

两块板**刷同一份固件**（`g3ce3bf6`，含当天的 MCAN ISR 修复）、**同一个测量二进制**
（`bridge_can_loopback_latency`，新增 `usb-5321` 模式就是为了这个）、同一台主机、
同样 `chrt -f 80` 绑核 7/6、同样锁死深 C-state。都是 CAN0 -> CAN1 回环、CAN-FD 8 字节。

| | HPM5321 DualCan（单核） | HPM6E8Y 单核镜像 | HPM6E8Y 核对调镜像 |
|---|---|---|---|
| p50 | 99.8 / 99.8 / 99.9 | 99.8 / 99.9 / 99.8 | 124.5 / 124.4 / 124.6 |
| p90 | 104.5 / 104.3 / 104.8 | 104.7 / 107.3 / 104.4 | 126.2 / 126.2 / 126.5 |
| p99 | 125.4 / 125.0 / 125.7 | 125.5 / 125.8 / 125.6 | 159.2 / 154.4 / 159.9 |
| max | 131.8 / 140.2 / 160.7 | 129.5 / 128.5 / 142.0 | 197.2 / 172.9 / 198.5 |

**结论（顺序很重要，别只看第一行）：**

1. **5321 和 6E8Y 单核镜像在每一个分位上都一样**，差值全在噪声内（p50 都是 99.8，
   p99 都是约 125.5）。**这条链路的延迟不由芯片决定。**
2. **原因**：延迟由 USB HS 微帧（125us）+ CAN-FD 线上时间（约 30us）主导，这两项
   两块芯片完全相同。6E8Y 的 600 MHz 双核、200 MHz AHB（5321 是 160 MHz，由固件里
   `kCanTimestampNsPerUs` 960 vs 1000 反推）、4 路 CAN，在这个指标上**买不到任何东西**。
3. **5321 看起来比 6E8Y 快 25us，是拿它跟核对调镜像比出来的假象**——那 25us 是
   6E8Y 释放 core1 之后的双核争用代价，不是芯片差距。

**选型含义**：如果需求就是"USB 转几路 CAN"，**5321 DualCan 在延迟上和 6E8Y 打平**，
6E8Y 的价值在于 **4 路 CAN、EtherCAT、双核**这三件功能，而不是更低的延迟。
为延迟去选 6E8Y 是选错了理由。

复现命令：

```bash
sudo ./host-tuning.sh --pmqos     # 另开终端
sudo chrt -f 80 ./host/build/examples/bridge_can_loopback_latency usb-5321 3000 7 6
sudo chrt -f 80 ./host/build/examples/bridge_can_loopback_latency usb      3000 7 6
```

## 多板方案选型：N 块 USB 板 vs EtherCAT 级联 [实测 2026-08-01]

"要多块板的话，为什么不用多块 5321，而要上 EtherCAT 级联？"——这是个对的问题，
实测答案是**多块 USB 板完全可行，但它最大的卖点（低延迟）恰好在变成多板的那一刻消失**。

| | 1 块 USB 板 | 2 块 USB 板（同一控制器） | EtherCAT 级联 |
|---|---|---|---|
| p50 | **99.8** | **124.7**（丢一个微帧） | 133.4 |
| max（调好 IRQ 后） | 130 | 180 | 161~271 |
| max（**没调 IRQ**） | 130 | **~1000** | — |
| 扩到更多块 | — | 同控制器越多越差；控制器数量有限（本机 2 个） | 周期随 PDO 总长增长，可预测 |
| 布线 | 1 根 | **N 根都要回主机** | **1 根菊花链下去** |
| 距离 | ~5 m | ~5 m | 每跳 100 m |
| **故障隔离** | — | **每块板独立，坏一块不影响其他** | **链路断则下游全掉** |
| 跨板同步 | 无 | 无 | DC，亚微秒 |
| 主机开销 | 1 个事件线程 | N 个事件线程 + 共享控制器中断 | **烧掉一整个核忙轮询** |
| 运维复杂度 | 低（插上就用） | 低 | 高（IgH 内核模块、SII/revision、仲裁） |

**怎么选：**

- **1 块板 → 用 USB 单核镜像。** 99.8us，没有任何理由上 EtherCAT。
- **2-3 块板 → 多块 USB 板仍然合理**，而且**故障隔离比 EtherCAT 好**。
  代价是 p50 退到约 125us（和 EtherCAT 基本持平），且**必须把 xHCI 中断线程提到
  FIFO 90**，否则 max 约 1 ms（见 [../../HOST_TUNING.md](../../HOST_TUNING.md) 第 9 节）。
- **EtherCAT 只在这三种情况下才划算**，且都与延迟无关：
  1. **布线**：一根线菊花链穿过机体，vs N 根线都要回主机（滑环、转台尤其明显）；
  2. **距离**：超过 USB 的约 5 m；
  3. **跨板同步**：多块板要在同一时刻动作（DC 亚微秒），USB 没有对应机制。

**不要为了延迟选 EtherCAT——实测它在任何板数下都不比 USB 快。**

## 主机侧延迟调优（每次重启都要重做）

```bash
sudo ./host-tuning.sh          # 应用：governor=performance、RT 限流关闭、USB autosuspend 等
sudo ./host-tuning.sh --check  # 只报告不改（还会打印本机的控制器、IRQ、P/E 核归属）
sudo ./host-tuning.sh --pmqos  # 另开一个终端，测量期间持住（只为最后约 2us 的 p99）
```

**除内核 cmdline 外全部不持久化。** 逐项状态、证据等级、以及被实测否掉的做法见
[../../HOST_TUNING.md](../../HOST_TUNING.md)（现行权威）第 1-3 节；
板级分布数据仍在 [ecat/DESIGN.md](ecat/DESIGN.md) 第 5 节和 3.6 节。

> 测 USB 延迟前**必须**先把主机弄到高频状态，否则测到的是电源管理不是传输：
> 主机路径上 p50 77.5 -> 67.5us、p99 约 100 -> 77us `[实测 2026-08-04]`；
> 板级 RTT 上则是 max 216.8 -> 160.7us（p50 被微帧量化，不动）。
>
> **但这一条只对"核会空闲"的负载成立。** 紧凑 ping-pong（`dual_board_test latency`
> 无采样间隔）下 governor 完全没有差别，powersave 自己就跑满频率；1kHz 控制环才是
> 会空闲的那一类。**别拿压测结论去判断控制环的主机调优**——判据与数据见
> [../../HOST_TUNING.md](../../HOST_TUNING.md) 1.1。

## 烧录脚本（仓库根目录）
```bash
./flash-ecat-swap.sh              # 核对调布局（主线）
CORE1=0 ./flash-ecat-swap.sh      # 单核 USB 镜像（USB 最快，p50 100us）
CAN_DIAG=1 ./flash-ecat-swap.sh   # 附带 CAN 转发遥测，配 host/examples/can_stall_probe
DIAG_USB=1 ./flash-ecat-swap.sh   # core1 日志经 USB 送出，配 host/examples/core1_log
./flash-ecat.sh                   # EtherCAT 桥旧布局（对照基线）
```

## CAN 采样点：必须对齐总线，不是对齐推荐表 [实测 2026-08-03]

**结论先行：`can.hpp` 把仲裁域和数据域的采样点都钉死在 87.5%，不要改回 SDK 默认，
也不要照厂商推荐表改。** 改动是 `Can` 构造函数里这四行：

```cpp
config.can20_samplepoint_min = 875U;   config.can20_samplepoint_max = 875U;
config.canfd_samplepoint_min = 875U;   config.canfd_samplepoint_max = 875U;
```

### 为什么必须显式设

HPM SDK 的默认窗口是 `[750, 875]`（千分比），而它的求解器**爬过下界就停**：

```c
while ((num_seg1 * 1000U) / num_tq < samplepoint_min) { ++num_seg1; --num_seg2; }
```

所以结果**永远是 75.0%**，那个 `max = 875` 是死代码。`bsp/hpm_sdk/samples/drivers/mcan/`
里一处都没设过这两个字段，**HPM 全部官方示例都跑 75%**。

### 症状与判据

CubeMX 板（mc02、c_board）两个域都是 `tseg1/tseg2 = 13/2 = 87.5%`。5321 在 75% 时：

| | 结果 |
|---|---|
| classic CAN 双向 | **正常**（1 Mbit 容差有 12.5 个百分点余量） |
| `mc02 -> 5321` FD | **正常** |
| `5321 -> mc02` FD | **一帧不到**，`PSR.DLEC = ACK error`，TEC 爬到 136 进 error-passive |

5 Mbit 一个位只有 200 ns，75% 与 87.5% 差 25 ns，接收方取到的已是下一位。
**这种"classic 通、FD 单向不通"就是采样点不一致的特征签名。**

### 关键陷阱：不要照推荐表改回去

厂商推荐表（">800 kbps 用 75%"、"仲裁域与数据域不要求一致"）会让你认为 1 Mbit
仲裁域应该是 75%。**照着做会坏。** 实测：仲裁域留 75%、只钉数据域 87.5% →
`PSR.DLEC = bit1 error`，双总线 **0/40000**。

原因是 mc02 的仲裁域**不是** 75%：

| | 仲裁域配置 | TQ 数 | 波特率 | 采样点 |
|---|---|---|---|---|
| 典型参考驱动 `CAN_BR_1M` | brp=1, seg1=59, seg2=20 | 80 | 1 Mbit | 75.0% |
| **mc02 实际（CubeMX）** | brp=5, seg1=13, seg2=2 | 16 | 1 Mbit | **87.5%** |

两者都是 1 Mbit，采样点差 12.5 个百分点。**推荐表是"没有其他约束时选什么"，
一旦总线上已有节点，"所有节点采样点一致"这条压倒一切。**

### 影响范围

`can.hpp` 由**全部 rmcs_board 镜像共用**（hpm5321 / hpm5321_dual_can / hpm6e8y，
含两套 EtherCAT 固件）。此前"FD 没问题"的印象来自只测 rmcs_board 对 rmcs_board——
两端错得一样所以互通。**真实电机（DJI、达妙 MIT、瓴控）全是 87.5%**，所以这个修复
是让 5321 能跟电机跑 CAN-FD，不只是为了跟 mc02 说话。

### 怎么自查

`-DLIBRMCS_CAN_DIAG=ON` 的遥测（记录版本 5）现在带 `NBTP`/`DBTP`，
`host/examples/can_stall_probe` 会直接打印解码后的采样点：

```
timing: nominal brp=1 tseg1=69 tseg2=10 sp=87.5% | data brp=1 tseg1=13 tseg2=2 sp=87.5% tdc=0
```

## UART 运行时改波特率：DLAB 会把 THR 变成除数锁存器 [实测 2026-08-05]

**结论先行：读 `DLL`/`DLM` 必须在 TX DMA 停稳之后，否则读的动作本身会毁掉波特率。**
`DLL` 与 `THR` 共用偏移 `0x20`、`DLM` 与 `IER` 共用 `0x24`，由 `LCR.DLAB` 选择
（`soc/HPM5300/ip/hpm_uart_regs.h`）。而 TX DMA 的目的地址在 init 时就固定成
`&uart_base_->THR`，**它不认识 DLAB**。所以只要 DLAB=1 期间 DMA 送出一个字节，
那个字节就写进了除数锁存器，波特率当场被数据覆盖，端口从此不出声。

### 症状签名（很反直觉，值得记住）

| 现象 | 说明 |
|---|---|
| 遥测报告除数**完全正确** | `DLM` 在 `DLL` 被覆盖**之前**读到，所以快照是好的 |
| 但那个口**一个字节都不发** | 硬件里的除数已经被数据字节改掉 |
| 与波特率精度**无关** | 1000000 / 2000000 除数精确（div=10 / div=5）照样失败 |
| 重刷两块板也没用 | 不是状态污染，每次 100 ms 遥测都会重新踩一次 |

**"算对了、写对了、然后不发"= 观测者把被观测对象改坏了。** 这次的观测者就是
`-DLIBRMCS_CAN_DIAG=ON` 里那段读除数的遥测代码，它跑在主循环、和 TX DMA 并发。

### 修法

- `Uart::snapshot_divisor()` 只在 **init** 和 **`handle_config()` 里 `abort_transmit()` 之后**
  采样一次，存进 `uart_divisor_`；遥测从快照读，**永不按需读寄存器**。
- `abort_transmit()` 先 `dma_abort_channel()` 再 `dma_mgr_disable_channel()`：
  `CHABORT` 的寄存器手册明确写"写入对未使能的通道会被忽略"[RM]，顺序颠倒等于没中止。
- 切换后补 `uart_reset_tx_fifo()`，丢掉旧波特率下已经进 FIFO 的字节。

### 上游怎么做的（对照过 SDK）

HPM SDK **全库没有任何读回 `DLL`/`DLM` 的代码**，也没有 `uart_get_baudrate()`。
唯一改运行时波特率的样例（`samples/drivers/uart/uart_lin/slave_baudrate_adaptive`）
是纯"只写不读"：`uart_set_baudrate()` 之后 `uart_reset_rx_fifo()` 再重开 RX。
**上游根本不回读，所以它碰不到这个坑。** 我们要回读是为了遥测，那就必须用快照。

另外 `uart_set_baudrate()` 自己有个缺陷值得知道：它**先**置 DLAB，求解失败时
**带着 DLAB=1 直接 return**，不清标志位（`drivers/src/hpm_uart_drv.c:222-227`）。
所以调用方必须无条件自己清一次 DLAB，不能只在成功分支清。

`uart_set_baudrate()` **是有返回值的**（`status_success` /
`status_uart_no_suitable_baudrate_parameter_found`），上游样例也确实检查它，而且
**失败时提前 return、跳过后面的 FIFO 复位**：

```c
hpm_stat_t stat = uart_set_baudrate(TEST_UART, lin_baudrate, uart_source_clk);
if (status_success == stat) { ... } else { printf("not supports"); return; }
uart_reset_rx_fifo(TEST_UART);   /* 只有成功才走到这里 */
```

我们照这个形状做：被拒时分频器没动、旧波特率仍然有效、队列里的字节对它仍然合法，
所以**跳过 FIFO 复位**（不为一次什么都没改的请求丢数据），直接返回 false。
这也和仓库内 `ch32_board` 的既成惯例一致——它对 BRR 范围做检查后同样返回 false，
注释讲得最直白：*"this is host-supplied data, so a bad value must be reported,
never asserted on"*。

### 已知缺口：配置被拒绝，主机看不到

`handle_config()` 现在返回求解器的真实结果（80 MHz 表示不出的波特率会被拒，
此时**分频器保持不动、端口继续用旧波特率**，不会损坏）。但
`link/host_session.hpp` 的 `uart_config_deserialized_callback` 丢弃了这个返回值并
无条件 `return true`——因为该回调的 `bool` 在协议层的含义是"这个 field 认不认识"，
不是"操作成不成功"，不能拿来传失败。**要让主机知道切换被拒，需要协议层加一条
config ack**，这是独立的功能缺口，尚未实现。

实测：请求 6000000（求解器无解）板端正确拒绝、寄存器未变、随后 115200 仍 PASS；
但主机侧不会打印 rejected。而 3000000 **会被接受**（`osc=26 div=1 -> 3076923`，
误差 2.56%，在 SDK 的 3% 容差内），别以为它非法。

### 顺带修掉的独立 bug

`abort_transmit()` 漏清 `in_flight_`，导致下一次 `try_dequeue()` 把 `out_` 推过
DMA 实际没发出的字节，环缓冲永久错位。**这个与上面的除数覆盖无关**：它只在切换
之后发作，且表现为"数据错乱"而不是"完全不发"。

## 教训：自环测试对"共模错误"是瞎的 [实测 2026-08-05]

**结论先行：同一块板两个口互接的自环测试，无法证明波特率切换真的生效。**
排查上面那个 bug 时，mc02 的 `UART7 <-> UART10` 自环在 115200 到 2000000 全部 PASS，
于是"mc02 被洗清、问题在 5321"——**这个推论是错的**。

自环会把两端**同时**设成目标波特率。如果配置代码根本没生效，两端就**一起**留在
115200，波特率相同、通信照样正常、测试照样 PASS。**自环只能验证两端一致，不能验证
两端等于你要的值。** 共模错误对它完全不可见。

实际上 mc02 这边还藏着第二个独立 bug：`HAL_RCCEx_GetPeriphCLKFreq()` 在本版 HAL 里
的 if/else 链只覆盖 SAI/SPI/ADC/SDMMC/FDCAN，**两个 UART 组一个分支都没有**，
直接掉到末尾 `else { frequency = 0; }` 返回 **0**（`stm32h7xx_hal_rcc_ex.c`）。
于是 `handle_config()` 撞上自己的 `kernel_clock_hz == 0` 提前返回，**`BRR` 一次都没写过**，
mc02 永远停在 CubeMX 的 115200。修法是照 `UART_SetConfig()` 自己 switch
`__HAL_RCC_GET_USART16_SOURCE()`，不要信那个函数。

**判据**：要证明"切换生效"，必须让**一端切、另一端不切**，然后确认通信**变坏**；
或者跨板对打，两端各自独立配置。只看自环 PASS 等于没测。

## 硬件中断与数据路径归属（6E8Y / 5321 共用同一份 app 代码）

问"硬件 ISR 都用上了吗"的答案：**延迟相关的事件全部是中断驱动的，没有该用中断却在轮询的地方。**

| 事件 | 上下文 | 优先级 | 备注 |
|---|---|---|---|
| CAN RX（MCAN0..3） | **ISR** | 3（最高） | 进 ISR 就排空 FIFO + 序列化，不甩给主循环 |
| USB（USB0） | **ISR** | 2 | `dcd_int_handler` 在 ISR 里处理硬件；TinyUSB 的回调（`tud_vendor_rx_cb`）按其设计延到主循环的 `tud_task()`——**代价 < 0.85us，见下** |
| UART RX/TX | **ISR** | 1 | |
| 1 kHz tick | **ISR**（MTIP） | 绕过 PLIC | 故意只做一个计数器自增，LED 等工作甩到主循环，避免抢占 CAN ISR |
| 跨核上行门铃 | **ISR**（MBX0B） | 低于 PDI | 核对调布局 |
| 跨核 flash RPC | **ISR**（MBX1A） | — | 不依赖主循环，见 `xcore/flash_server.hpp` |
| ESC PDI（core1） | **ISR** | 4 | |
| CAN TX 完成 | **不启用中断** | — | 发完无事可做，启用只是白加中断 |
| DMA | **数据路径未用** | — | `dma_mgr_init()` 调了但没接数据面；MCAN 确实是 DMAMUX 源（`HPM_DMA_SRC_MCAN0..5`），见 [ecat/LINKX_HW_ACCEL_PLAN.md](ecat/LINKX_HW_ACCEL_PLAN.md) 2.1 |

**主循环周期实测 0.72us（空闲）/ 0.85us（16000 f/s 满载）**，所以"等下一趟主循环"这件事
最多值 0.85us。对照 RTT p50 124.8us，**板端整条路径不到 3%**。
完整论证与"哪些板端优化因此不值得做"见
[../../HOST_TUNING.md](../../HOST_TUNING.md) 第 8 节。

## 板上没有调试器时怎么看现场

本板的调试口（FT2232：串口 + JTAG）通常不接，本机也没装 OpenOCD。**不要因此认为
"看不到现场"**——两条带内通道已经入库，都是走 USB vendor 端点、编成 UART0 上行帧：

| 想看什么 | 固件开关 | 主机工具 |
|---|---|---|
| CAN 转发是否在走：ISR 进入计数、MCAN `IR`/`RXF0S`/`PSR`/`ECR`、PLIC pending/enable/trigger | `-DLIBRMCS_CAN_DIAG=ON` | `host/examples/can_stall_probe.cpp`（边压测边解码，转发停摆时打印前后快照） |
| core1（EtherCAT 核）在说什么：channel 版本、SII/EEPROM 决策、SSC 起来没有、`ecat_time_ms` 心跳 | `-DLIBRMCS_DIAG_OVER_USB=ON` | `host/examples/core1_log.cpp` |

两个开关都默认 OFF：前者会在 CAN 热路径上加计数器，后者会占用 `DataId::kUart0`
（本板的 UART1 数据口）。2026-08-01 的 CAN 闩死定位和 core1 时基确认全部靠这两条
通道完成，没有用到任何调试器。

## 构建
```bash
export PATH=~/3rd_party/hpm/bin:$PATH        # [前机路径] 确保 riscv32-unknown-elf-gcc 可见
# USB 数据固件（超级构建，含 app + bootloader）
cmake --preset debug -S firmware/rmcs_board
cmake --build firmware/rmcs_board/build       # target: rmcs_board_app / rmcs_board_bootloader
# hpm5321_dual_can（单核 USB + 2 路 CAN）
cmake --preset release -S firmware/rmcs_board -B <build> -DBOARD=hpm5321_dual_can
# 烧录：dfu-util -d 0xa11c:0xa902 -a 0 -D <build>/app/output/rmcs_board_app_hpm5321_dual_can.dfu
# EtherCAT 桥固件（旧布局）
cmake --preset debug -S firmware/rmcs_board/ecat
cmake --build firmware/rmcs_board/ecat/build
# 核对调布局（core0=USB+CAN / core1=EtherCAT）。先建 core1 再建 core0，超级构建已排序
cmake --preset release -S firmware/rmcs_board -DBOARD=hpm6e8y -DLIBRMCS_RELEASE_CORE1=ON
cmake --build firmware/rmcs_board/build --target rmcs_board_app
```
- preset：`debug` / `debug-outside` / `release`（注意本板 `CMAKE_BUILD_TYPE` 用小写 `debug`/`release`）。
- 构建需 Python 3 + `PyYAML`、`jinja2`（HPM SDK 代码生成用）。

## 目录结构
- `app/`、`bootloader/`：USB 固件两半。`app/src/xcore/`：核对调布局下 core0 的跨核部分
  （环主机侧、次核装载、flash RPC 服务端），单核构建下整体编译为空。
  `app/generated/`：core1 镜像的 C 数组落点（构建产物，入库，同 `ecat/core0/src/sec_core_img.c` 惯例）。
- `ecat/`：EtherCAT 桥（独立超级构建 `rmcs_ecat_superbuild`）。`ecat/core1_ecat/`：核对调布局的
  core1 镜像（纯 EtherCAT）。`ecat/common/`：跨核契约（`xcore_channel.hpp` 带版本号，
  core0/core1 镜像必须同版本，不匹配时 core1 会停机而不是静默错乱）。
- `boards/`、`common/`：板级配置与共享代码。`bsp/hpm_sdk`：HPM SDK submodule（第三方，只读）。
- 无 CubeMX，不适用 CubeMX 纪律。

## 备注
- 缺工具链的机器（如 build server）只能编 host SDK；固件需在装了 RISC-V 工具链的 PC 上编/烧。详见 `BUILD_ENVIRONMENT.md`。
