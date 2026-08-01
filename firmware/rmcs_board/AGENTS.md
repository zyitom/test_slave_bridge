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

> **选型要点（实测 2026-08-01，各 3 轮，别凭直觉也别凭单次测量选）**：
>
> | | p50 | p99 | max（3 轮） |
> |---|---|---|---|
> | USB 单核镜像 | **99.8** | **125.6** | 128.5 ~ 142.0 |
> | USB 核对调镜像 | 124.8 | 146.8 ~ 155.7 | 159.7 ~ **250.7** |
> | EtherCAT（IgH） | 133.4 | 140.5 | 161.4 ~ 177.1 |
>
> **单核 USB 在每一个分位上都赢，包括尾部，而且三轮高度一致。** 只要一块桥板够用、
> USB 线长够用，它就是最优解。EtherCAT 换来的不是速度，是**确定性**（p99-p50 只有 7us）
> 和多从站/长距离能力——固定延迟能在控制器里补偿，随机抖动不能。
>
> 核对调镜像的 USB 尾部**不可复现**（1/3 的轮次 max 跳到 250），这是它最需要注意的性质。
>
> 完整分布、C-state 的影响、以及两条被否掉的主机侧调优见
> [ecat/DESIGN.md](ecat/DESIGN.md) 3.5 / 3.6 节；被推翻的假设见
> [CORE_SWAP_MIGRATION.md](ecat/CORE_SWAP_MIGRATION.md) 4.6 节。

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
  FIFO 90**，否则 max 约 1 ms（见 [../../HOST_TUNING.md](../../HOST_TUNING.md) 4.3）。
- **EtherCAT 只在这三种情况下才划算**，且都与延迟无关：
  1. **布线**：一根线菊花链穿过机体，vs N 根线都要回主机（滑环、转台尤其明显）；
  2. **距离**：超过 USB 的约 5 m；
  3. **跨板同步**：多块板要在同一时刻动作（DC 亚微秒），USB 没有对应机制。

**不要为了延迟选 EtherCAT——实测它在任何板数下都不比 USB 快。**

## 主机侧延迟调优（每次重启都要重做）

```bash
sudo ./host-tuning.sh          # 应用：RT 限流关闭、governor、网卡 rx-usecs/EEE、USB autosuspend
sudo ./host-tuning.sh --check  # 只报告不改
sudo ./host-tuning.sh --pmqos  # 另开一个终端，测量期间持住（关深 C-state）
```

**除内核 cmdline 外全部不持久化。** 逐项状态、证据等级、以及两条被实测否掉的调优
（governor 无效、xHCI 中断与事件线程绑同核**有害**）见
[ecat/DESIGN.md](ecat/DESIGN.md) 第 5 节的表和 3.6 节。

> 测 USB 延迟前**必须**先跑 `--pmqos`，否则测到的是电源管理不是传输：
> 同一测试开关它的差别是 max 216.8 -> 160.7 us。

## 烧录脚本（仓库根目录）
```bash
./flash-ecat-swap.sh              # 核对调布局（主线）
CORE1=0 ./flash-ecat-swap.sh      # 单核 USB 镜像（USB 最快，p50 100us）
CAN_DIAG=1 ./flash-ecat-swap.sh   # 附带 CAN 转发遥测，配 host/examples/can_stall_probe
DIAG_USB=1 ./flash-ecat-swap.sh   # core1 日志经 USB 送出，配 host/examples/core1_log
./flash-ecat.sh                   # EtherCAT 桥旧布局（对照基线）
```

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
[../../HOST_TUNING.md](../../HOST_TUNING.md) 4.2 节。

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
