# rmcs_board 固件指南

> **文档类型**：现行规范（板级）
> **适用范围**：`firmware/rmcs_board/`，HPMicro HPM6E8Y / HPM5321（Andes RISC-V）
> **状态**：现行有效
> **相关文档**：[仓库根 AGENTS.md](../../AGENTS.md) · [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md)（完整环境搭建） · [USB_OPTIMIZATION_LOG.md](USB_OPTIMIZATION_LOG.md)（USB 调优实录：做过什么、否掉了什么） · [ecat/README.md](ecat/README.md)（EtherCAT 桥）

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
  fork `zyitom/hpm_sdk`，当前提交 `v1.12.0-3-ge4347411`，分支 `migrate-v1.12.0`。
- 所有 rmcs_board 镜像（app、bootloader、旧布局 EtherCAT core0）统一编译共享的
  `firmware/c_board/bsp/tinyusb` **v0.21.0**，接入点是 `cmake/current_tinyusb.cmake`；
  HPM SDK 自带的 TinyUSB v0.20.0 及 fork 内旧补丁不再进入镜像。构建前必须初始化该
  TinyUSB submodule；HPM SDK 仍提供 SoC、USB PHY 与寄存器驱动。

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

**第三条烧录途径：经 EtherCAT FoE**（核对调布局，已上板验证）。主站把镜像推进板上的
暂存区，冷复位后由 bootloader 校验并装入 app 槽——不需要 USB，适合够不到板子的场合。

```bash
# 载荷既不是 .bin 也不是 .dfu，是 .dfu 砍掉末尾 16 字节的 DFU 后缀
python3 -c "d=open('firmware/rmcs_board/build_hpm6e8y_swap/app/output/rmcs_board_app_hpm6e8y.dfu','rb').read(); open('/tmp/fw.img','wb').write(d[:-16])"
sudo ethercat states BOOT && sleep 3     # 必须确认真的到 BOOT 再发
sudo ethercat foe_write -o app /tmp/fw.img
sudo ethercat states PREOP               # 退出 BOOT 触发冷复位 + 安装
```

**前提是 SSC 带 FoE**：需要用 `ecat/tools/HPM_ECAT_RMCS_Config.xml` 重新生成，
仓库「构建前提」描述的原样例程配置不含 FoE。完整设计、实测时序、载荷格式陷阱和
被推翻的判断见 [ecat/FOE_FIRMWARE_UPDATE.md](ecat/FOE_FIRMWARE_UPDATE.md)。

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

`can.hpp` 由**全部 rmcs_board 镜像共用**（hpm5321 / hpm6e8y，
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

## TinyUSB 0.21 迁移后的 jitter 调优：事件预算是惰性的 [实测 2026-08-06]

**结论先行：`CFG_TUD_TASK_EVENTS_PER_RUN` 在本固件上调不动任何分位，不要再去 A/B 它。**
它是 0.21 新增的旋钮（0.20 的 `tud_task_ext()` 是 `while(1)` 无界排空），看起来像是
"限制 USB 事件连续占用、给 CAN TX 让路"的正解，实际上一次都没被触发。

### 为什么惰性（结构性原因，换负载也不会变）

主循环一圈 0.72us（空闲）/ 0.85us（满载），即 `tud_task()` 每秒被调用约 120 万次；
而事件到达率只有几 kHz。**队列里几乎永远不超过 1 个事件，4 / 8 / 16 的上限碰都碰不到。**
只有主循环被阻塞到毫秒量级才可能堆积到 16 个，本固件没有任何这样的路径。

### 实测数据

两块 HPM5321 DualCan 对打（CAN0↔CAN0、CAN1↔CAN1、UART0 互接），
`dual_board_test contend 15000 20000 2 4 6 8`，`RMCS_LATENCY_GAP_US=1000`，
主机按 `host-tuning.sh` 调好，各 2 轮：

| 事件预算 / 队列 | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| 16 / 16（默认） | 124.9 / 125.0 | 142.4 / 142.8 | 147.3 / 147.5 | 149.7 / 149.7 |
| 4 / 32 | 125.1 / 125.2 | 142.5 / 142.9 | 147.0 / 147.3 | 149.1 / 150.3 |

同一配置重复跑的离散度约 ±0.5us，两档之间的差值全部落在这个范围内。8/32 在无背景
负载的 `latency` 上同样与默认重合。**因此 tusb_config.h 里不设这两个宏**——写一个等于
默认值的 override 只是死配置。

> `contend` 的最后一个参数是 `load_core`，**必须绑核**。不绑（默认 -1）时负载线程
> 与测量线程抢核，背景只发得出约 3000 帧，测到的是主机调度而不是板端行为。

### 别拿 CAN 压测去证明 USB 吞吐没变

`dual_board_test stress` 在拐点上基线与改后完全重合（≤19000 f/s 零丢失；20000 f/s
bus0 丢 0.82~0.91%；21000 f/s 丢 5.49~5.62%），bus0 收到的帧数无论送多少都停在约
198200 / 10 s。**但这条约 19.8k f/s 的天花板是 CAN 线速，不是 USB。**
`stress` 每次迭代把 can0+can1 批进**一个** USB 包，所以 19000 f/s 每总线只产生
19000 packets/s——USB 事务上限根本没被碰到。用它得出"USB 吞吐没变"是无效论证。
`usb_packet_rate.cpp` 的文件头早就写明了这个陷阱，仍然值得在这里再说一遍。

### USB 裸包率：这批改动唯一测得出的收益 [实测]

`sudo usb_packet_rate split3 0 10`（flood，每条记录独占一个包），各 3~5 轮，
单轮离散度 ±0.07%：

| 固件 | packets/s | 相对基线 |
|---|---|---|
| 基线 | 59216 | — |
| 仅 dcache 编译掉 | 58879 | **−0.57%** |
| 仅 ctz 端点扫描 | 59471 | +0.43% |
| **两者（当前）** | **59953** | **+1.24%** |
| 再加 ILM | 60018 | +1.35% |

各档区间互不重叠，不是噪声。**但注意这四行不可加**：单独 +0.43% 与 −0.57% 合起来
本该是 −0.14%，实测却是 +1.24%。这种强交互是**代码布局 / I-cache 对齐**的签名，
不是省下的指令数。所以正确的说法是"当前这份源码在这块芯片上跑出 +1.24%"，
**不能说成"因为少执行了 N 条指令所以快了"**——换个编译器版本或改动上游无关代码都可能翻盘。
`[实测]`；机制 `[推断，未做指令级剖析]`。

同一批改动在主循环周期上是精确中性（见下），两者不矛盾：主循环每秒 78 万圈里绝大多数
是空转，USB 事件每秒只有 6 万次；`usb_packet_rate` 的 flood 把后者拉满，`hpm5321_loop_probe`
的 10000 f/s 负载没有。**测哪个量，取决于你关心的是 CPU 余量还是包率上限。**

### 那 60k packets/s 到底由什么构成 [实测 2026-08-07]

**结论先行：包率上限是端点周转，不是 CPU、不是总线带宽、也不是主机。板端 CPU 在
上限处只有 34% 占空比，其余是端点空等。**

**(1) 不是总线带宽。** USB 2.0 spec 那个"每微帧 13 个 bulk 事务"是 **512 字节** payload
的数字，本协议一条记录只有 8 字节 payload、整包约 16~24 字节。HS bulk 事务固定开销约
352 bit（token 64 + DATA 头尾 64 + ACK 48 + 两次包间隔约 176），加 8N bit 数据：

| payload | 事务耗时 | 每微帧 |
|---|---|---|
| 512 B | 4448 bit = 9.27 us | 13.5（对上 spec 的 13，模型自洽 ✓） |
| 16 B | 480 bit = **1.0 us** | 约 125 |

实测 7.5 个/微帧 = **总线利用率约 6%**。**别把 512 字节的表格套到小包上**，那会把余量
低估一个数量级。

**(2) 不是主机。** `usbmon` 抓 bus 3 两秒（37 万行，压测仍报 60053 packets/s，开销不显著）：

| 管道 | n | p50 延迟 | p90 | p99 |
|---|---|---|---|---|
| Bulk OUT | 119796 | **1062 us** | 1104 | 1118 |

Little's law：59898 包/s × 1062 us = **63.6 个 URB 在飞**，正好是主机侧 64 个 transfer
池的容量。**队列一直是满的，主机始终有 64 个 URB 排着等设备收。**
两板并发（A 52434 + B 51999 = 104433，若纯板端限制应约 120k，若纯主机限制应约 60k）
进一步确认：**per-device 为主，主机争用只占约 13%**。

**(3) 也不是 CPU。** `hpm5321_loop_probe` 扫描负载取斜率（空转周期
100000/89892 = 1.1124 us）：

| 负载形态 | 斜率（迭代/单位负载） | CPU 成本 |
|---|---|---|
| 只压下行 | 5.28 | **5.87 us / 下行包** |
| 双向往返 | 10.8 | 12.0 us / 往返 → 上行侧约 6.1 us |

而 `split` 模式的上限 58297 packets/s = **17.15 us / 包**。

```
17.15 us  每包实际占用
 -1.0 us  总线时间
 -5.9 us  板端 CPU
========
~10.2 us  端点既没在收也没在算 —— 周转死时间（占 60%）
```

**占空比 34%。**

> **以下这段推算已被实测推翻，保留原文备查**（见下一节）：
> ~~所以"链多个 dTD 让控制器不等 CPU 重新 prime"这个方向是打在点上的，理论天花板由
> CPU 决定，约 1/5.87us ≈ **170k packets/s**（约 2.9 倍）。~~
> 错在把整个 10.2 us 死时间都当成设备侧可回收的。**仍然成立的部分**：17.15 us 的
> 每包预算、1.0 us 总线时间、5.9 us CPU 成本、34% 占空比——这些都是实测，没有变。

**但代价要看清**：硬件支持每端点 8 个 qTD（`hpm_soc_feature.h:87`），TinyUSB 只排 1 个
（`dcd_ci_hs.c:160` 注释 "for portability"），而且 `usbd.c` 的 `usbd_edpt_busy` 是每端点
每方向**一个 BUSY 位**——**DCD 和 usbd 两层都要改**，是上游项目不是补丁。

### 那 10.2 us 死时间里，设备侧只占约 3.5 us [实测 2026-08-07]

**结论先行：multi-qTD 的天花板是约 +20% 包率，不是 2.9 倍；而且它修不了下一节那个
head-of-line blocking。上一节"设备 NAK 与控制器重试节奏没有分离"的缺口，现在补上了，
不需要硬件抓包器。**

做法是在 `can_diag` 记录（版本 6）里加两个 `CSR_MCYCLE` 时间戳，把 OUT 端点的每包周期
就地拆成两段——`turnaround` = 完成 → 重新 arm 完毕（**设备侧，multi-qTD 唯一能消掉的**），
`starve` = arm 完毕 → 下一次完成（端点已就绪却空等，是总线时间加主机控制器发下一个
token 的节奏）。用 CSR 而不是 mchtmr 是因为它是单条指令，不会污染被测对象。
解码在 `host/examples/hpm5321_loop_probe.cpp`。

| 负载 | 每包周期 | turnaround（设备侧） | starve（主机+总线） |
|---|---|---|---|
| 11761 packets/s | 85.03 us | **3.81 us** | 81.22 us |
| 21763 packets/s | 45.95 us | **3.11 us** | 42.84 us |

**负载翻倍，`turnaround` 不涨反微降**——它是与负载无关的纯设备侧代码路径，所以这个值
可以外推到饱和点。对着饱和时的 17.15 us 每包预算，设备侧只占 **18~22%**，即
multi-qTD 最多把 58k 推到约 **70k packets/s**。

**更要紧的推论**：下一节实测的"一次 UART 撞击约 +40us"，按 2 × 17.15us（完整包周期，
上下行各一次）解释得通，按 2 × 3.5us（设备侧周转）解释不通。**所以那 40us 是包在主机
控制器排队里多等的，不是等设备重新 prime——multi-qTD 消不掉它，分端点才能**（两对端点
在控制器那里独立调度，CAN 包根本不排在 UART 包后面）。

#### 饱和点补测：81% 的每包预算花在等主机 [实测 2026-08-07]

上面两个点都在 12k / 22k packets/s，`starve` 被低负载撑大，不能当饱和时的数。补测的做法
不是给 `hpm5321_loop_probe` 加 flood——它**故意不 flood**，因为 `start_transmit()` 在
transfer 池空时会阻塞，而阻塞中的发送线程看不到 `running`，`join()` 就会挂（rate=60000
时实测挂死）。改为把解码器抽到 `host/examples/can_diag_record.hpp`，让**已经拥有正确
flood 的 `usb_packet_rate`** 也读这条遥测。

`usb_packet_rate split3 0 10`，单管道（`LIBRMCS_SPLIT_CAN_ENDPOINT=0`，这样全部 57.7k
包都经过被仪表化的那条管道），三轮：

| | 值 | 占比 |
|---|---|---|
| 每包周期 | 18.4 us | — |
| **turnaround（设备侧）** | **3.47 / 3.47 / 3.47 us** | **19%** |
| **starve（主机侧 + 总线）** | **14.85 / 14.98 / 14.97 us** | **81%** |

**结论：设备已经 arm 好、在那儿等，而下一个包要 15 us 才来。** 减掉约 1.0 us 总线时间，
**约 14 us 是主机控制器在设备就绪之后才把下一个包送出来**——主机侧那 64 个 URB 一直排满
（usbmon 实测），所以不是"主机没活干"，是 xHCI 把包交出来的节奏。

#### 因果实验：`cycle = turnaround x 1.2~1.4 + 约 13.5 us` [实测 2026-08-07]

上面那个"设备侧只占 19%"本身不足以推出 multi-qTD 值多少——还要知道**主机那 15 us 是不是
被设备的空窗触发的**。做法是在 `tud_vendor_rx_cb` 里、re-arm 之前插入一段可配置的忙等
（`CSR_MCYCLE` 计数），人为拉长 turnaround，看 cycle 怎么动。节流同时短路掉，避免设备
自己故意不 arm 污染数据。六个点：

| 注入延迟 | turnaround | starve | cycle | 包率 |
|---|---|---|---|---|
| 0 | 3.19 | 15.0 | **18.4** | 54.3k |
| 600 cy | 4.53 | 14.5 | **19.1** | 52.5k |
| 1200 cy | 5.89 | 15.3 | **21.2** | 47.1k |
| 1800 cy | 7.11 | 17.4 | **24.5** | 40.8k |
| 2400 cy | 8.37 | 16.3 | **24.7** | 40.5k |
| 4800 cy | 13.43 | 11.6 | 25.0 | 40.0k |

**turnaround 在 3.2~8.4 us 区间内，`starve` 基本是常数（14.5~17.4 us），cycle 跟着
turnaround 走，斜率 1.2~1.4。** 最后那个 4800 点已经出了有效区间——13.4 us 的忙等让
设备自己成了瓶颈（starve 反而缩到 11.6），不能入拟合。

**两个结论：**

1. **multi-qTD 的收益是 +23% ~ +35%**（外推到 turnaround=0：cycle 13.5~14.5 us，
   54k → 69~74k packets/s）。此前从低负载外推的 +20% 是**保守端**，可以引用。
   `[实测区间内线性]`；外推到 0 仍是 `[推断]`。
2. **约 13.5 us 的截距是主机侧地板**，设备侧碰不到。换更快的 MCU、ILM、链 qTD，
   全都只能在斜率那一项里做文章。

**顺带纠正上一节一个归因错误**：背压那 4.4% 包率代价，之前写的是"更像代码布局 / I-cache"。
**不对**。斜率 1.2~1.4 意味着 **`rx_cb` 路径上每多一微秒，包率就直接掉一截**——节流那次
队列遍历把 turnaround 拉长了约 0.65 us（60222 → 57536 即 16.6 → 17.4 us/包，除以斜率），
在 460 MHz 上约 300 个周期，和"遍历两个 CAN 控制器 + 迟滞判断"完全对得上。
**机制找到了，不是玄学。** 推论：**任何加进 `rx_cb` 的工作都按 1.2~1.4 倍换算成包率损失**，
这条比"少执行 N 条指令"那种直觉可靠得多。

> **仪表盲点**：时间戳只埋在 `tud_vendor_rx_cb` 的 itf-0 分支。分端点开启时 CAN 走
> itf-1，于是遥测只反映承载 UART 的那条管道（实测报 18k packets/s 而主机报 57k）。
> 要测饱和必须把 split 关掉，或另外给 CAN 管道埋一套。

> 测量注意：速率必须压在 CAN-FD 约 19.8k f/s 天花板以下。实测 20000 那一点
> iters/100ms 从 82385 塌到 35691——那是 TX 队列打满后的行为，不在直线上，不能入拟合。

### UART 会顶掉 CAN 的尾部延迟：head-of-line blocking 实测 [实测 2026-08-07]

**结论先行：CAN 和 UART 共用一条 bulk 管道，一次撞上 UART 数据的 CAN 帧要多等约 40us。
p50 几乎不动，但 p99 从 104.5 涨到 144。而且只要 25 kB/s（线速的 27%）就吃掉九成损失——
不需要打满，任何现实的 UART 用量都在付这个代价。**

`dual_board_test uartcontend 20000 <kB/s> 2 4 6 8`，测量路径与 `latency 0` / `contend`
完全相同；对照组是 `contend 1 20000 2 4 6 8`（背景 1 f/s，等效空闲）：

| UART 负载 | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| 0（对照） | 100.6 | 103.2 | **104.5** | 118.7 | 124.8 |
| 10 kB/s | 100.7 | 103.5 | **120.3** | 141.3 | 145.6 |
| 25 kB/s | 101.1 | 110.2 | **138.4** | 144.9 | 156.7 |
| 50 kB/s | 102.0 | 119.1 | 141.4 | 145.1 | 149.5 |
| 75 kB/s | 102.9 | 119.9 | 141.8 | 145.1 | 158.8 |
| 92 kB/s（线速） | 103.3 | 120.4 | **143.9** | 159.9 | 163.6 |

**不是 CPU 争用，是排队。** 10 kB/s 只有 156 个 chunk/s，按每包约 5.9us 的 CPU 成本算
不到 0.1% 占用，却已经让 p99 涨了 15.8us。这个量级的额外负载只可能通过序列化生效。

**命中率模型对得上每一行**：测量是紧凑 ping-pong，约 10000 样本/s；UART chunk 率除以它
就是被撞概率，而撞一次的代价固定约 +40us（≈ 两个端点周转，见上一节实测的每包约 17us）：

| UART | chunk/s | 被撞比例 | 于是先动的分位 |
|---|---|---|---|
| 10 kB/s | 156 | 1.6% | p99（1% 尾）刚好开始动 |
| 25 kB/s | 390 | 3.9% | p99 吃满，p90 开始动 |
| 92 kB/s | 1560 | 15.6% | p90 也吃满 |

**这是 0.21 的 `CFG_TUD_VENDOR_EP_INT_IN` 唯一有实测依据的用途**：给 CAN 单开一个端点，
UART 就排不到它前面。0.20 的 vendor class 没有这个能力，做不了。

> 测量陷阱：两次运行之间必须留约 10 s 让板端 UART 队列排空，否则后一次会带着前一次的
> 积压跑，表现为大量 CAN 超时（实测 n 从 20000 掉到约 7000）。分位数本身不怎么受影响，
> 但样本数会。另外负载线程的定速**必须钳位** `next`，`next += period` 一旦落后于当下就
> 会退化成 flood——实测踩过一次：sent 24 MB / delivered 86 kB，13101 次超时。

### 包率的真正上限：主机每微帧只发 8 个 bulk 事务 [实测 2026-08-07]

**结论先行：单板包率上限是 64000 packets/s = 8000 微帧 x 8，六轮离散度 0.01%。这是主机
控制器的调度量子，per-device，与板端做什么完全无关。所有"链 qTD / 缩短 turnaround /
加端点"的方向到此全部关闭——设备每 17 us 就能交出一个包，而主机每 15.6 us 才要一个。**

> **但"包率上限"不等于"吞吐上限"，别把这一节当成天花板。** 这个 64000 是用 8 字节
> payload 测的，只有 0.41 MB/s；把 payload 加到 448 字节，URB 率只掉到 35668，吞吐涨到
> **15.98 MB/s**。限制是**每秒多少次传输**，不是每秒多少字节。完整的 payload 扫描、
> 512 字节整包边界的陷阱、以及"到底还剩多少余量"见
> [USB_OPTIMIZATION_LOG.md](USB_OPTIMIZATION_LOG.md) 第 1 节。

此前所有包率测量都用 `usb_packet_rate split3`，而它每次迭代发两条 CAN——**被 CAN 线速
限住了**，测到的 57~60k 不是 USB 上限。新增 `uartonly` 模式（不含任何 CAN，UART 队列满了
就丢字节，USB 包照发）才测得到端点本身：

| 测法 | packets/s |
|---|---|
| `split3`（含 CAN） | 57.6k ← **被 CAN 线速限** |
| **`uartonly` 单板** | **63988~64002** |
| **`uartonly` 双板** | **128880**（A 64609 + B 64271） |

**双板正好翻倍**，所以量子是 per-device，控制器远没有饱和。

**排除了 host transfer 池**：把 `kTransmitTransferCount` 从 64 改成 32 / 128 / 256，包率
全部停在约 50.5k（同一固件），**与池大小无关**。64000 恰好等于 64 池 x 1000 帧/秒，是巧合。

### 分端点的真实代价：峰值包率 −21% [实测 2026-08-07]

**同一份交付配置，只切 `LIBRMCS_SPLIT_CAN_ENDPOINT` 一个开关：**

| | packets/s |
|---|---|
| `SPLIT=0` | **64012 / 64008 / 64012**（离散度 0.006%） |
| `SPLIT=1` | **50792 / 50377 / 50868** |

**分端点吃掉 21% 的峰值包率**，即数据端点从 8.0 掉到约 6.3 事务/微帧。机制 `[推断]`：
第二对端点的 IN 侧挂着 16 个 URB 却没有数据可发，主机持续轮询、设备持续 NAK，这些轮询
占掉了微帧里的调度槽。

> **上一节说分端点"包率持平"是错的**，那是用 `split3` 测的——它被 CAN 线速限在 57.6k，
> 天花板在 64k，所以看不见这 21%。**要测端点本身必须用 `uartonly`。**

**但这个代价落在用不到的余量上**：任何带 CAN 的场景都被 19.8k f/s 的线速限死，够不到
50k 更够不到 64k。所以这笔交易是"**拿摸不到的 21% 峰值，换 UART 负载下 21 us 的 p99**"，
仍然划算。真要拿回来就把开关关掉，host 会自动探测不到接口 1 并回落到单管道。

> 顺带纠正上一节的一处归因：那里写"双板 104433，主机争用约 13%"。**不对**，那 13% 是
> CAN 线速，不是控制器争用——CAN-free 双板是干净的 2 倍。

### 由此关闭的两个方向

**1. multi-qTD 不会有任何收益。** 它唯一能改善的是设备侧 turnaround，而 turnaround 已经
不在关键路径上。直接实验：把 `rx_cb` 改成"先 memcpy 出来、立刻 arm、再处理"
（`LIBRMCS_COPY_THEN_ARM`，vendor.cpp），turnaround 2.22 -> 1.27 us，**包率一模一样**：

| | turnaround | starve | host 包率 |
|---|---|---|---|
| 原顺序（处理完再 arm） | 2.22 us | 14.96 us | 63999 / 63996 / 64002 |
| copy-then-arm | **1.27 us** | 15.72 us | 63988 / 63988 / 63991 |

**省下的 0.95 us 原封不动变成了 starve。** 该开关因此**默认 0**——只有在"每微帧超过 8 个"
的主机上，设备侧才会重新变成约束，那时它才有意义。

**2. 之前那个"注入延迟"实验的线性模型是混杂的，作废。** 它得出
`cycle = 1.2~1.4 x turnaround + 13.5 us`，据此推出 multi-qTD 值 +23~35%。**错在忙等同时
增加了"未 arm 窗口"和"每包 CPU 总量"两个变量**，无法归因。这次只搬动工作位置、CPU 总量
不变，cycle 纹丝不动——证明起作用的是后者（或干脆两者都不是），不是前者。
**教训：改一个变量。**

### USB OUT 背压：把静默丢帧换成无损降速，代价 4.4% 包率 [实测 2026-08-07]

**结论先行：CAN 过载时板子不再静默丢帧，而是不 arm OUT 端点、让控制器 NAK 把主机压回
CAN 线速。代价是 USB 峰值包率 −4.4%，延迟每一个分位都不变。开关是
`CFG_TUD_VENDOR_RX_MANUAL_XFER`，置 0 整段编译消失。**

`tusb_config.h` 打开 manual xfer 后，class driver 不再自动重新 arm，改由
`usb::Vendor::poll_downlink_arm()` 决定。门控看 `can::max_transmit_queue_depth()`：
到 48/64 停止 arm，掉回 16/64 才恢复（迟滞，否则会在队列最深处抖动）。

| 场景 | 基线 | 背压后 |
|---|---|---|
| 19000 f/s | 零丢失 | 零丢失 |
| 21000 f/s | bus0 丢 **5.49~5.62%** | **0.0000%** |
| 25000 f/s | — | **零丢失**，10 s 的活自动跑成 13.4 s |
| latency p50/p90/p99/p99.9/max | 100.6/103.2/104.5/118.7/124.8 | 100.7/103.5/104.5/**111.1**/**117.8** |
| `usb_packet_rate split3` | **60222** | **57560（−4.4%）** |

**为什么不是无损保证**：一个 512 字节 OUT 包最多装 46 条 8 字节 CAN 记录（每条 11 字节）
或约 170 条 DLC=0 的，而队列只有 64 深，且队列开始涨时主机已有最多 64 个包在飞。所以它
限的是**稳态速率**，不是让溢出不可能。把水位按最坏包来设要压到队列 1/4 以下，正常突发
都会被误伤。

**必须有的逃生门**：bulk OUT 同时承载 UART 下行和 session keepalive。CAN bus-off 时队列
永不排空，无限期扣住 arm 会把整条链路一起弄死。所以扣满 20 ms 就放弃背压（健康总线排空
64 槽只要约 3.2 ms，session lease 是 1000 ms），宁可丢帧也不丢会话。

**那 4.4% 的机制**：拆开看是 manual-xfer 机制本身 −2.23%（门控短路后 58879）、门控再
−2.28%。两个基于"少执行几条指令"的假设都被实测否掉——把 re-arm 从主循环挪进 `rx_cb`
（57508/57554/57555，无变化）、把每包两次队列查询减成一次（57562/57555/57561，无变化）。
**真正的机制在下一节的因果实验里找到了**：`cycle = turnaround x 1.2~1.4 + 常数`，所以
代价不取决于指令条数，而取决于**这些工作有没有落在 `rx_cb` 到 re-arm 这段路径上**。
上面两次改动都只是把工作在该路径内挪位置，没有移出去，所以无效。

### 给 CAN 单开一对 bulk 端点：削掉约一半 head-of-line blocking [实测 2026-08-07]

**结论先行：CAN 走自己的 bulk 端点对（0x02/0x82）后，UART 线速下 p99 从 143.9 降到
122.9~128.3；25 kB/s 下从 138.4 降到 122.0~122.5。但 10 kB/s 那一档没有改善，p90 在高
负载下也几乎不动——所以它削掉的是约一半，不是全部。** 开关 `LIBRMCS_SPLIT_CAN_ENDPOINT`
（`app/include/tusb_config.h`），**默认开**。

用的是**第二个 vendor interface = 第二对 bulk 端点**，不是 0.21 的
`CFG_TUD_VENDOR_EP_INT_IN`。interrupt 端点按 `bInterval` 轮询，高速下会把上行量化到
125us 微帧；bulk 保持原有的连续轮询行为，只是不再共享排队。

| UART kB/s | p90 单管道→分端点 | **p99 单管道→分端点** | p99.9 单管道→分端点 |
|---|---|---|---|
| 0（对照） | 103.2 → 101.0 | 104.5 → 105.0 / 117.3 / 117.5 | 118.7 → 116.9~120.7 |
| 10 | 103.5 → 101.3 / 101.4 | **120.3 → 120.9 / 120.0** | 141.3 → 123.1 |
| 25 | 110.2 → 105.1 / 109.8 | **138.4 → 122.0 / 122.5** | 144.9 → 126.7 / 129.6 |
| 92（线速） | 120.4 → 119.3 / 119.6 | **143.9 → 122.9 / 128.3** | 159.9 → 133.2 / 141.5 |

> **对照组的 p99 是双峰的**：同一份固件重复三次得到 105.0 / 117.3 / 117.5。所以
> "削掉了百分之多少"这种说法给不出可信区间，只有加载后的绝对值可比。**别拿单次对照
> 去算差值**——我为此追过一次并不存在的"退化"。

**没被削掉的那一半是什么**：p90 在 92 kB/s 下 119.3 vs 单管道 120.4，几乎不动，说明撞击
仍在发生，只是代价小了。剩下的共享资源有两处，都不在 USB 端点上：主机侧**单个 libusb
事件线程**串行处理两个端点的完成回调，以及板端**单个主循环**串行跑 `tud_task()`、CAN 排空
和两条管道的 pump。`[推断]`——没有分离测量。

### 分端点是个交叉点，不是纯赚：空载 p99 反而 +11 us [实测 2026-08-07]

**结论先行：分端点让 p50/p90 变好、p99 在空载时变差约 11 us、在 UART 超过约 10 kB/s 时
大幅变好。交叉点在约 10 kB/s，所以该不该开取决于这块板的 UART 实际用不用。**

**本项目的选择：开（默认 1）**，因为这块板的 UART0 持续跑数据，工作点在交叉点右侧。
UART 长期接近空载的场景应当把它关掉——`LIBRMCS_SPLIT_CAN_ENDPOINT=0`，host 会自动探测
不到接口 1 并回落到单管道，不需要同步改主机。

同一个空载对照（`contend 1 20000 2 4 6 8`），只切开关：

| | p50 | p90 | **p99** | p99.9 |
|---|---|---|---|---|
| `SPLIT=0`（5 轮） | 100.6 | 103.3 | **105.1~111.0** | ~118 |
| `SPLIT=1`（8 轮） | **99.7** | **101.0** | **117.6~118.2** | ~122 |

**分布形状也不同**：`SPLIT=1` 时 90% 的样本挤在中位数 1.3 us 内，然后 p99 突然跳 **17 us**
——约等于一个主机服务间隔（15.6 us，见上一节）。`SPLIT=0` 时 p90 -> p99 只跳 3.7 us，
分布是平滑的。机制 `[推断]`：第二个端点的轮询偶尔把 CAN 端点的服务推迟一个槽。

**和 UART 负载扫描并排看，交叉点就出来了：**

| UART | `SPLIT=0` p99 | `SPLIT=1` p99 | 谁赢 |
|---|---|---|---|
| 0 | **~107** | 118.0 | 不分端点 |
| 10 kB/s | 120.3 | 120.0 | 打平 |
| 25 kB/s | 138.4 | **122.0** | 分端点 |
| 92 kB/s | 143.9 | **122.8** | 分端点 |

> **此前"分端点纯赚 p99"的说法是错的**，因为对照组只跑了三次（105.0/117.3/117.5），
> 我把那个 105 当成了噪声。8 轮复测证明 `SPLIT=1` 稳定在 118，`SPLIT=0` 稳定在 105~111
> ——**是真差异，不是噪声**。教训：对照组也要跑够轮数。

### 三个必须知道的坑

**1. 背压必须跟着 CAN 走，否则整个失效。** CAN 下行改到优先管道后，如果只在 bulk 管道上
保留节流，`stress 25000` 会从零丢失退回 **20.5% 丢失**——帧从一条永远不说不的管道进来。
所以 `poll_can_downlink_arm()` 现在带节流，`poll_downlink_arm()` 的节流则在分端点时
编译掉（那条管道已经没有 CAN 队列要保护）。

**2. 主循环里 CAN 要排在 bulk 前面。** 一趟主循环内的调用顺序是板端唯一的优先级机制。
92 kB/s 下 CAN 优先 p99 123.7，bulk 优先 128.4。对照组两者相同。

**3. libusb 那句"close 会可靠取消所有传输"的注释是错的。** 第二对端点暴露了它：
`libusb_close()` 之后 32 个 RX transfer 里有 6~16 个仍未完成（先 release 接口是 6 个，
不 release 是 16 个），`handle_events()` 于是死等一个永远到不了 0 的计数，表现为**测量
跑完了、进程不退出**。正确顺序是 **cancel → 等收割 → 再 close**，`handle_events()` 还要
有界超时兜底。这是 host 侧既有的隐藏 bug，单接口时因为 EP 0x81 一直有数据流动才没踩到。

> **调试提示**：`dual_board_test` 的结果走 stdout，重定向后是全缓冲的。进程被 timeout
> 杀掉时缓冲区丢失，看起来像"什么都没跑出来"，实际上测量早就完成了。用 `stdbuf -oL`。

**代价**：`usb_packet_rate split3` 57621 / 57623，与单管道加背压的 57560 持平（都低于
无背压的 60222，那 4.4% 是背压的代价，见上一节）。UART 完整性 PASS，`stress 25000`
零丢失 PASS。

### 同批改动的取舍（都在共享的 `firmware/c_board/bsp/tinyusb`）

| 改动 | 位置 | 结论 |
|---|---|---|
| 恢复 ChipIdea SETUP tripwire（SUTW 信号量重试环） | `dcd_ci_hs.c` | **保留**，正确性回归修复，见下 |
| `CFG_TUD_MEM_DCACHE_ENABLE=0` 时把 `dcd_dcache_*` 编译掉 | `dcd.h` / `usbd.c` | 保留，与下一项合计 +1.24% 包率 |
| ISR 端点扫描改为只遍历 `ENDPTCOMPLETE` 的置位 | `dcd_ci_hs.c` | 保留，同上（两者缺一都更差） |
| ~~USB 热路径加 `TU_ATTR_FAST_FUNC` 进 ILM~~ | — | **已撤销**：包率 +0.11%，主循环 −4.0% |

### 不要把 TinyUSB 热路径放进 ILM

给 `dcd_int_handler` / `tud_task_ext` / `process_edpt_complete_isr` / `vendord_xfer_cb`
加 `TU_ATTR_FAST_FUNC`（HPM 上即 `.fast` 段，本板链接脚本把它映射到 ILM）看起来是
稳赚的——ILM 零等待、省掉 FLASH XIP 的 I-cache miss。**实测是净倒退。**

`hpm5321_loop_probe`，两块板双向各 10000 f/s，各 15 s（149 条遥测记录，离散度 0.15%）：

| 固件 | 空载主循环周期 | 满载主循环周期 |
|---|---|---|
| 基线（迁移后未改） | 1.115 us | 1.280 us |
| 仅 tripwire + dcache + ctz | 1.113 us | **1.280 us** |
| 再加上 ILM | 1.140 us | **1.331 us（+4.0%）** |

中间那行证明另外三项在这个量上精确中性，倒退全部来自 ILM 这一项。

**为什么 ILM 在包率上 +0.11% 却在主循环上 −4.0%**：`tud_task()` 每秒被主循环调用约
78 万次，其中绝大多数是"队列空、立刻返回"，而真正有事件要处理的只有约 6 万次。
把 `tud_task_ext` 搬进 ILM，等于让那 78 万次空转全部付一次跨段调用，去换 6 万次工作
路径的零等待取指。**代价按 78 万次收，收益按 6 万次算**，所以净亏。

**机制**：ILM 基址 `0x0000_0000`，FLASH XIP 的 `.text` 在 `0x8000_0000`，相距 2 GB；
而 RISC-V `jal` 的射程只有 ±1 MB。把 `tud_task_ext` 搬进 ILM，等于把它和它每次调用的
一堆仍在 flash 里的类驱动回调劈到跨 2 GB 的两侧，**每一次跨段调用都从 `jal` 退化成
`auipc`+`jalr`**。搬进去省下的取指时间，抵不过整条调用链多出来的长跳转。
`[推断]`——地址布局与 `jal` 射程是硬事实 `[RM]`，但 4% 里各占多少没有做指令级剖析。

**推论**：`.fast` 只适合**叶子函数或自成闭环的调用子图**（板级 `.fast` 里现有的 CAN
转发胶水就是这一类）。像 `tud_task_ext` 这种"入口在热路径、被调方散落在 flash"的
函数，搬进 ILM 只会制造跨段调用。`LIBRMCS_ILM_HOT_PATH`（只对 HPM6E8Y 生效）搬的是
SDK 的 MCAN 叶子驱动，不是这一类，两者不要混为一谈。

**第一项是真回归，不是上游老毛病**：HPM SDK 自带的 `dcd_hpm.c` 实现了
`set_sutw` / `get_sutw` 重试环，而 0.21 通用的 `dcd_ci_hs.c` 清完 `ENDPTSETUPSTAT`
就直接把 qhd 指针交出去——`USBCMD_SETUP_TRIPWIRE`（`ci_hs_type.h`，HPM 上是
`USBCMD` bit 13 [RM]）在该文件里定义了却没人用。迁移等于丢掉了一层保护，背靠背
SETUP 会让 usbd 拿到撕裂的 8 字节。只影响枚举/控制传输，不影响稳态 jitter。

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
| 主循环周期（板端 CPU 余量的直接读数） | `-DLIBRMCS_CAN_DIAG=ON` | `host/examples/hpm5321_loop_probe.cpp` |

> `can_stall_probe` **在 HPM5321 上跑不了**：它绑死 HPM6E8Y 的 PID `0xA904`，并按
> 单板自环驱动 CAN0->CAN1 与 CAN2->CAN3，假设四路总线。5321 只有两路，双板 rig 又是
> 交叉对接而非自环。`hpm5321_loop_probe` 补的就是这个缺口——只解码遥测里的主循环
> 计数，负载自带（两块板双向对发），因为压测工具会独占两块板，遥测读端再也开不进去。

两个开关都默认 OFF：前者会在 CAN 热路径上加计数器，后者会占用 `DataId::kUart0`
（本板的 UART1 数据口）。2026-08-01 的 CAN 闩死定位和 core1 时基确认全部靠这两条
通道完成，没有用到任何调试器。

## 构建
```bash
export PATH=~/3rd_party/hpm/bin:$PATH        # [前机路径] 确保 riscv32-unknown-elf-gcc 可见
# USB 数据固件（超级构建，含 app + bootloader）
cmake --preset debug -S firmware/rmcs_board
cmake --build firmware/rmcs_board/build       # target: rmcs_board_app / rmcs_board_bootloader
# 两块 HPM5321 板（单 CAN / 双 CAN-FD）共用 -DBOARD=hpm5321 这一个镜像，上电自己判板型
cmake --preset release -S firmware/rmcs_board -B <build> -DBOARD=hpm5321
# 烧录：同一个 .dfu，只有 -d 的 PID 按板子填（单 CAN a901 / 双 CAN-FD a902）
# dfu-util -d 0xa11c:0xa902 -a 0 -D <build>/app/output/rmcs_board_app_hpm5321.dfu
# HPM5321 单 CAN / 双 CAN-FD 共用 -DBOARD=hpm5321，板型由 OTP 第 25 字判断。
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
