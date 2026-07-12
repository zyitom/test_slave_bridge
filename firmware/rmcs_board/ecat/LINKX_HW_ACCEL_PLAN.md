# linkx 硬件加速最终规划（HPM6E80 / hpm6e8y 转发板）

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
