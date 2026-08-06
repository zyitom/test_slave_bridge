# linkx 硬件加速最终规划（HPM6E80 / hpm6e8y 转发板）

> **文档类型**：背景说明（实施规划）
> **适用范围**：`firmware/rmcs_board/ecat/`，HPM6E80 硬件加速
> **状态**：现行有效（规划已定稿，上板验证清单见第 3 节，尚未全部完成）
> **相关文档**：[DESIGN.md](DESIGN.md)（更上层的选型论证） · [README.md](README.md)（当前实现） · [../boards/hpm6e8y/README.md](../boards/hpm6e8y/README.md)（板级参考）

## 摘要

本文件是**下一阶段的实施规划**：在 HPM6E80 上把哪些数据搬运工作从 CPU 卸载到硬件、
每一项的依据（手册章节 + 寄存器级确认）、以及**评估后主动排除了哪些硬件**（第 2 节，
记下原因是为了避免以后反复讨论同一件事）。

定优先级的准绳是第 0 节那张延迟账：**CAN classic 一帧上线的 110-130 µs 才是全链路最大
项**，所以 CAN-FD 排在所有板内 CPU 优化之前。不为省 1 µs 的环节增加 bring-up 面积。

依据：HPM6E00 用户手册 Rev0.8（HPM6E00UMV08.pdf）逐章核实 + SDK 头文件寄存器级确认。
目标：EtherCAT ↔ 4×CAN 转发的最低延迟。所有结论以"CPU 从数据路径上消失"为方向，
但以延迟账为准绳——不为省 1 µs 的环节增加 bring-up 面积。

## 0. 延迟账（决定优先级的事实）

| 环节 | 延迟 | 结论 |
|---|---|---|
| 帧过站（ESC on-the-fly） | <1 µs | 硅上，无事可做 |
| PDI ISR 消费 SM2 → 排 CAN TX | ~1-2 µs | P3 分核后已贴硬件极限 |
| CAN classic 1 Mbps 一帧上线 | ~110-130 µs | **全链路最大项** |
| CAN-FD 数据段 5 Mbps 一帧 | ~25-30 µs | 第一优先级的那一刀 |
| master cycle 量化 | 几十 µs 起 | master 侧主战场 |

优先级：**CAN-FD > 双 PDO 分域 > master 侧压缩 >> 板内继续抠 CPU**。

> **上表的最后一行已被实测取代 [2026-08-01]**：master cycle 实测
> **25.75 µs（38.8 kHz，0 wc error）**，不是"几十 µs 起"；而端到端 RTT p50 157.5 µs
> = **约 6.1 个周期**。所以 master 侧的战场不是"把周期压短"（已经很短了），
> 而是**减少每次往返消耗的周期数**。CAN-FD 也已经在用了（经典 ~110 µs → FD ~30 µs
> 这一刀已经吃到）。完整校准见 [DESIGN.md](DESIGN.md) 4.1 节。
>
> 对本文档第 1 节的影响：1.1 的 SYNC-DMA 自评"预期收益 1-2 µs + 抖动确定性"**依然成立，
> 也依然不是大头**——它省的是 PDI ISR 那 1-2 µs，不是那 6 个周期。要动周期数，
> 先做 hybrid 固定 PDO（去掉 ARQ 的 ack 往返），再谈 DMA。

## 1. 采用的硬件加速（已核实证据）

### 1.1 SYNC-DMA 零 CPU 过程数据流水线（双 PDO latest-wins 域，v2）

- 证据：手册 67.2.7 `ESC.GPR_CFG1[SYNCn_DMA_EN]`；SDK `hpm_dmamux_src.h`
  `HPM_DMA_SRC_ESC_SYNC0 = 0x76`、`ESC_SYNC1 = 0x77`；手册 33.4.2 DMA 链式传输
  （内存驻留描述符链，处理器不介入）；ESC 映射在 `0xF1700000`（`hpm_soc_ip.h`），
  DMA 可寻址。
- 下行：master 开 DC 后，SYNC0 → HDMA 描述符链：PDRAM 设定值区 → MCAN message
  RAM → 链尾描述符直接写 TXBAR 寄存器 → CAN 帧上线。PDI ISR 从下行路径消失。
- 上行：SYNC1 相位配在读帧飞过前几微秒 → DMA 把本地快照区整块拷入 PDRAM 输入
  映射。新鲜度确定、零 CPU、天然满足 SM3 单写者。
- 约束：只覆盖 latest-wins 域（固定布局）；ARQ 字节流域仍走 PDI ISR；free-run
  模式退回软中断合并刷新。
- 落地方式：**v1 布局按 DMA 友好设计（固定偏移、区块对齐、CAN 帧序即内存序），
  用 CPU 拷贝实现；量出数字后 v2 换 DMA**（预期收益 1-2 µs + 抖动确定性）。

### 1.2 全硬件时间同步链（DC 域 ↔ CAN 时间戳，零 CPU/帧）

- 证据：SDK `hpm_trgmmux_src.h`：`TRGM0_INPUT_SRC_ESC_SYNC0 = 0x4`、
  `ESC_SYNC1 = 0xF`、`PTPC_CMP0/1 = 0x6/0x7`；`TRGM0_OUTPUT_SRC_MCAN_PTPC0_CAP =
  0xE0`、`MCAN_PTPC1_CAP = 0xE1`、`ESC_TRIG_IN(latch0) = 0xF1`。手册 63.2.2 PTPC
  输入捕获（CAPT_SNAPH/L + 中断）、63.2.3 PTPC 4 个时间戳输出端口；
  `hpm_mcan_soc.h` TSU 外部时基 `TBSEL_0..3`（对应 PTPC 4 个输出端口）。
- 链路 A（DC → PTPC 校准）：ESC SYNC0 → TRGM → PTPC 捕获输入，SYNC0 沿的 PTPC
  时刻被硬件快照；软件只需周期性读快照对 PTPC 做 syntonize，得到 DC 相关时基。
- 链路 B（CAN 帧打戳）：PTPC 时间戳输出端口 → MCAN TSU 外部时基，每帧收发瞬间
  由硬件打 DC 相关时间戳。时间戳从"ISR 里读定时器"变成"硅上帧沿锁存"，
  精度提一个量级、每帧零 CPU。
- 链路 C（反向 latch，可选）：PTPC_CMP → TRGM → ESC latch0，把本地事件时刻
  打进 DC 时间域，用于诊断/测量。

### 1.3 DMA 资源划分

- HDMA（AHB，低延迟）归 core1：SYNC-DMA 流水线、快照批拷贝。
- XDMA（AXI，高带宽）归 core0：EEPROM store 页搬运、USB 诊断流批拷贝。
- DMAMUX 通道分配表进 board/，constexpr 编译期定死，杜绝运行时争用。
- 证据：手册 31.4 / 33 章（XDMA、HDMA 各 32 请求，编程模型相同，DMAMUX 可路由
  到任意 DMA 控制器）。

### 1.4 USB 与安全子系统（core0，非实时路径）

- USB HS 设备控制器自带 dQH/dTD DMA：端点搬运零 CPU，core0 只碰描述符（现状已用）。
- SDP（手册 75 章）：SHA-256 校验 DFU 镜像（刷写路径 CPU 不算哈希）；其内置 DMA
  支持数据拷贝/块拷贝/填充，可给诊断流大块搬运兜底。仅 core0 使用。
- TRNG：session nonce、serial 盐值。
- 明确推迟：PKA + KEYM + OTP 签名链（OTP 不可逆、动全板共享 bootloader 的 DFU
  故事，v2 再议）；EXIP 实时解密（无 IP 保护需求不开）。

### 1.5 跨核与监督（既有机制，规划确认）

- MBX：TX/RX FIFO 各 4 字、A/B 双口双中断（手册 35.2.2）——控制面（心跳、
  reboot-to-DFU、模式切换、EEPROM 请求/完成）+ 上行 doorbell。
- SHARE_RAM：PMP 非缓存 + RV32A AMO（既有 board_init_pmp 做法），跨核环
  acquire/release + 设备写前 full fence 纪律照搬。
- 跨核数据环**常驻但 best-effort**：满即丢、绝不背压数据面；承载开发模式
  usb_link 与生产模式诊断镜像（镜像拷贝计入 core1 CAN ISR 时序预算）。
- WDG×2 分层：core1 WDG 由事件循环喂（超时 → core0 经 MBX 心跳发现 → 单独
  SW 复位 core1）；core0 WDG 绑 tud_task 循环（僵死 → 整机回 bootloader）。
- MCHTMR×2 各核私有（MTIME 读法用 upstream #66 的 32 位 MMIO 视图）。

### 1.6 ESC 板级功能转正（bring-up 补丁 → 正路）

- **warm-restart：`ESC_RST_PDI` 寄存器**（手册 67.2.4）。core1 单独复位重启后自己
  写 ESC_RST_PDI 复位 ESC + PHY，再走冷启动——不破坏"core0 绝不碰 ESC"铁律。
  复位请求可触发中断（`GPR_CFG1[RSTO_IRQ_EN]`）。
- link 状态：`GPR_CFG2[NMII_LINKn_FROM_IO]` + CTR MUX 选 IO 接 PHY link LED
  （可反相），替代当前 MDIO 轮询 BMSR 的 hack；未用端口按手册用 GPR 压成 No Link。
- 端口指示灯：用 ESC 的 LINKACT 信号（经 CTR MUX 映射 + 反相）驱动，
  而非 PHY LED（手册 67.2.5 明确建议）。
- EEPROM emulation 是默认模式（`GPR_CFG0[EEPROM_EMU]` 复位值即模拟），与
  "core1 回调经 MBX 转发、core0 落盘"方案兼容；写挂起语义仍需上板验证（见 §3）。

## 2. 评估后排除的硬件（记录原因，避免反复）

| 硬件 | 排除原因 |
|---|---|
| TSW（千兆 TSN 交换）+ ENET | 与 ESC 互斥的上行路线；v2 可做"EtherCAT 生产 + 以太网诊断"双栈，v1 关闭，只在 board/ 记录引脚归属 |
| PLB | 电机换相用的位级逻辑，桥上无此需求 |
| FFA | FFT/数字滤波加速器（手册 32 章），与转发无关 |
| CRC 单元 | 8 通道、寄存器喂数（手册 36 章，无 DMA 喂数路径）；48B ARQ chunk 配置开销 vs 软件查表收益存疑——默认软件 CRC，实测后再换 |
| GPTMR 做 15 灯 PWM | 逆向出的 LED pad（PE03/04/05 等）大概率无 TMR 输出复用，通道数也不够；软件 LED 只花 1 kHz tick 零头 |
| ESC 64 路 GPIO / GPO 锁存 | 为纯数字 IO 从站设计的位级过程数据，桥用不上 |
| SYNC EVTO/EVTI 引脚输出 | 无外部同步器件需求；SoC 内走 TRGM 已覆盖 |
| 外挂 I2C EEPROM | 模拟模式为默认且满足需求，省 BOM 和引脚 |

## 2.1 外设利用率普查 [实测 2026-08-01]

按 SDK 的 `HPMSOC_HAS_HPMSDK_*` 清点，HPM6E8Y 共 **67 个外设**，固件用到 **18 个**：

```
已用: ESC MCAN USB PTPC MBX MCHTMR UART GPIO GPIOM PLIC SYSCTL
      PLLCTLV2 PMP OTP PCFG MULTICORE ENET DMAV2(仅 dma_mgr_init，未进数据路径)
```

**"没用满"不是问题**——未用的 49 个里绝大多数（ADC16/ACMP/DAO/I2S/PDMLITE/SEI/
QEIV2/QEOV2/PWMV2/PLB/FFA/FEMC/MTG/SPI/I2C…）是给电机控制、模拟采集、音频这类
本转发桥不做的事准备的，第 2 节已逐条记过排除理由。

**真正值得记的只有四项**，前三项是性能、第四项是**可靠性缺口**：

| 外设 | 现状 | 价值 | 出处 |
|---|---|---|---|
| DMA（HDMA/XDMA + DMAMUX） | `dma_mgr_init()` 调了，但**数据路径一次都没用** | SYNC-DMA 流水线，省 PDI ISR 那 1-2 us | 本文 1.1 |
| TRGM | 未用 | PTPC <-> ESC SYNC 的硬件时间同步链 | 本文 1.2 |
| SDP | 未用 | DFU 镜像 SHA-256 硬件加速 | 本文 1.4 |
| **EWDG / WDG** | **完全没有任何看门狗代码** | **本文 1.5 规划过两级 WDG 监督，从未实现。这是生产可靠性缺口，不是性能问题** | 本文 1.5 |

**第四项应当被当成待办而不是"已评估排除"**：core1 跑飞、core0 主循环卡死这两类故障
目前都没有硬件兜底，只能靠人拔电。GPIO_LED 逆向文档里那条"DFU detach 长跑后失效"
的恢复办法是"按住 KEYA 复位"——那正是一个本该由看门狗覆盖的场景。

> 顺带纠正一个容易搞错的：`GPTMR0` 被 SDK 的 ecat port 层用作 `ecat_time_ms` 的时基
> （`hpm_ecat_hw.c` 里 `clock_add_to_group(ECAT_TIMER_GPTMR_CLK, 0)`），所以它**在用**，
> 只是不出现在本项目源码的 include 列表里。

**HPM5321 DualCan 的情况不同**：芯片有 **4 路 CAN**（数据手册 1.1 节），板子只接了
**2 路**（MCAN0 = PA00/PA01，MCAN3 = PA31/PA30）。这是 **QFN48 封装的引脚可用性**限制，
不是固件选择，详见 `../boards/hpm5321/PINOUT.md`。想要 4 路 CAN 必须换到
HPM6E8Y 或更大封装。

## 2.2 CAN-FD 数据段速率：上限实测与最终结论 [实测 2026-08-01]

手册 62.1 写着 CAN-FD "数据速率可达 8Mbit/s"，而固件里 `kCanFdDataBaudrate` 是
**5 Mbit**。看起来是白放着的性能，实测走了一遍：

| 数据段速率 | 板对板回环结果 |
|---|---|
| 5 Mbit（现状） | PASS |
| 6 Mbit | **PASS**，30 秒 8000 f/s 压测 240000/240000、0 损坏 |
| 7 Mbit | **rx=0**，一帧都不通 |
| 8 Mbit | **rx=0**，一帧都不通 |

**电气上限在 6~7 Mbit 之间。** 6 Mbit 的单帧 RTT 收益很有意思但**不可靠**：
p50 五轮为 124.4 / 105.3 / 100.6 / 100.6 / 100.3——**要么约 100 要么约 124，随轮次跳**。
原因是省下的约 3us CAN 线上时间恰好把回复顶到 **USB 125us 微帧边界上**，落在哪一侧
由温漂、时钟相位这类不可控因素决定（同一现象见
[../../../HOST_TUNING.md](../../../HOST_TUNING.md) 第 10 节）。

> **最终结论：维持 5 Mbit，本条已关闭。** 原因不是芯片或板子——是**电机侧最高只支持
> 5 Mbit**。CAN 速率是全总线属性，由总线上最慢的设备决定，桥这一端再快也没用。
>
> 保留这段记录是为了让下次有人看到"手册写 8 Mbit"时不必再测一遍：
> **板子能到 6，电机只能到 5，所以是 5。**

## 2.3 还值得做的：MCAN TX Event FIFO（当前未用）

在"看门狗暂不做、引脚已由硬件定死、CAN 速率被电机卡在 5 Mbit"这三条约束之后，
**手册里剩下最值得用的一个外设能力是 MCAN 的 TX Event FIFO**，SDK 已支持
（`mcan_config_t::enable_tx_evt_fifo`、`mcan_read_tx_evt_fifo()`、
`mcan_tx_event_fifo_elem_t`），**本项目一处都没引用**。

**为什么它对"PID 跑在上位机"这个架构特别重要**：

- 接收方向**已经有**硬件时间戳（TSU 走 PTPC0，SOF 时刻锁存，见 `app/src/can/can.cpp`），
  所以"反馈是什么时候到的"是准确已知的。
- 发送方向**什么都没有**。主机把命令交给固件之后，帧进 32 深的 TX FIFO，然后等 CAN
  仲裁——**它到底什么时候上的线，现在完全不可知**。
- 一条总线一拍要发 7 帧时，最后一帧可能比第一帧晚 6 x 30us = 180us 上线。
  **这段排队延迟正是 28 电机 tick 预算里最不确定的部分**，而它现在是盲区。

TX Event FIFO 给每个已发送帧一个时间戳，**和 RX 用的是同一个 PTPC 时基**，所以可以直接
相减。做完之后能第一次回答："我这一拍的命令实际是什么时候上线的、排队花了多久、
到反馈回来一共多久"——这是决定控制周期能不能收紧的那个数，比任何传输层优化都更该先有。

其余仍未用但价值有限的：**MCAN DMA**（`HPM_DMA_SRC_MCAN0..5` 确实存在，但 CAN ISR 本身
只占 1-2us，省不出多少）、**专用 RX buffer + ID 过滤器**（把每个电机 ID 路由到独立缓冲，
省一点 ISR 时间）、**TRGM**（只有开了 DC 才有意义）、**TSNS**（板温遥测，与延迟无关但
机器人上有用）。

## 3. 上板验证清单（按风险排序，非依赖顺序）

1. **EEPROM 异步落盘语义**——SSC emulation 能否容忍 store 经 MBX 转发异步完成
   （ESC EEPROM busy/ack 位处理）。全方案单点风险，最先做。
2. core1 收一次 ESC SM 事件的最小镜像（PLIC 编号 SoC 级，理论可行，上板确认）。
3. core1 链接 SSC 全量的 ILM 余量（ILM/DLM 各 256K；现 core0 全家桶 143K、
   core1 45K，预计 ~130K，大概率过，早测）。
4. SM3 合并刷新原型：整区写触发三缓冲换页、限频逻辑正确性。
5. **HDMA → ESC PDRAM 可达性 + DMA 尾字节写能否触发 SM 换页**（SYNC-DMA
   流水线的前提）。
6. **ESC_RST_PDI warm-restart 路径**：core1 重启后接管活 ESC（复位 → AL 降
   INIT → 重走状态机），master 侧观察 PD watchdog 行为。

## 4. 两核分工定稿（背景，详见对话结论）

- core0 = 监管核：USB（DFU/usb_link/诊断）、flash/XPI 唯一写者、EEPROM 落盘、
  心跳监督与 core1 单独复位、LED、WDG。宽松主循环，允许阻塞。
- core1 = 数据核：ESC + PDI/SYNC 中断、SSC（防腐层包住）、ARQ、serdes、4×MCAN、
  PTPC/TSU 读取。全镜像驻 ILM，纯事件驱动 + WFI。
- 铁律：外设单属主核；flash 单写者 = core0；SM3 单写者 = core1 唯一刷新路径；
  跨核环 best-effort 不背压。
- 中断优先级（core1，高→低）：SYNC0/1 → PDI → CAN RX → 快照刷新软中断 → MBX。
