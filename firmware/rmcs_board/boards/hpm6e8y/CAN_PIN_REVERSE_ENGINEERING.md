# HPM6E8Y CAN 引脚逆向

> **文档类型**：过程记录（硬件逆向）
> **适用范围**：`firmware/rmcs_board/boards/hpm6e8y/`，无原理图的回收板
> **状态**：现行有效（四路 CAN 引脚均已实测确认）
> **相关文档**：[README.md](README.md)（板级说明） · [GPIO_LED_REVERSE_ENGINEERING.md](GPIO_LED_REVERSE_ENGINEERING.md) · [ETHERNET_PIN_REVERSE_ENGINEERING.md](ETHERNET_PIN_REVERSE_ENGINEERING.md)

## 摘要

日期：2026-07-09。

这块板**原理图缺失**，引脚只能靠扫描实测反推。本文件记录 CAN 引脚扫描器的结果：
**四路物理 CAN 干净地对应 `MCAN0` ~ `MCAN3`，引脚全部确认**（见下表）。

测试方法：外接一个 USB 转 CAN 适配器，持续发送 ID 为 `0x123` 的标准 CAN 帧；固件里的
扫描器逐个尝试候选的 MCAN 引脚复用组合，一旦某个候选 RX 收到了 `0x123`，就用同一候选的
TX 回一帧作为应答。

扫描器应答帧的格式：

| 字节 | 含义 |
| --- | --- |
| 0 | magic，恒为 `0xC5` |
| 1 | 候选编号 |
| 2 | MCAN 序号 |
| 3 | TX 所在 bank 的 ASCII |
| 4 | TX 引脚号 |
| 5 | RX 所在 bank 的 ASCII |
| 6 | RX 引脚号 |
| 7 | 阶段，`0x52` 表示 `R` / 已接收 |

应答帧的 CAN ID 是 `0x680 + 候选编号`。

## 已确认的 CAN 映射

| 板上丝印 | 原始应答 ID | 原始负载 | 候选编号 | HPM 外设 | TX 引脚 | RX 引脚 | 状态 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CAN0 | `0x690` | `C5 10 00 43 00 43 01 52` | `0x10` / 16 | `MCAN0` | `PC00` | `PC01` | 收发均确认 |
| CAN1 | `0x689` | `C5 09 01 42 05 42 04 52` | `0x09` / 9 | `MCAN1` | `PB05` | `PB04` | 收发均确认 |
| CAN2 | `0x69A` | `C5 1A 02 44 08 44 09 52` | `0x1A` / 26 | `MCAN2` | `PD08` | `PD09` | 收发均确认 |
| CAN3 | `0x69B` | `C5 1B 03 44 0F 44 0E 52` | `0x1B` / 27 | `MCAN3` | `PD15` | `PD14` | 收发均确认 |

## 分析

四路物理 CAN 口干净地映射到 `MCAN0` 到 `MCAN3`。**板上丝印顺序也与外设序号顺序一致**，
尽管引脚分散在 `PB`、`PC`、`PD` 三个 bank 上。

这些命中项全部落在主 A-F IOC bank 候选集里，不在 V/W/X/Y/Z 扩展候选集里。这一点与
"较小 BGA 封装"的假设是自洽的。

本次测试**不需要 CAN STB 引脚**。收发器看起来要么一直使能，要么由板上其他逻辑使能。

对照目前已知的 hpm6e8y 板级配置，这些 CAN 引脚不与下列资源冲突：

- 这块板用到的 USB0 引脚：`PF19`、`PF22`、`PF23`。
- 通常位于 PA bank 的 JTAG 引脚。
- 当前配置的 EtherCAT MII 引脚，主要在 `PA`、`PB12`-`PB23`，以及
  `PE02`/`PE03`/`PE06`。

目前常规的 EtherCAT 桥只把物理 CAN0 暴露为逻辑 CAN0。剩下的物理 CAN1..CAN3 已确认可用，
将来主机侧桥接 API 支持多路 CAN 时可以直接放出来：

| 逻辑 CAN | 外设 | TX | RX |
| --- | --- | --- | --- |
| `can0` | `HPM_MCAN0` | `PC00` | `PC01` |
| `can1` | `HPM_MCAN1` | `PB05` | `PB04` |
| `can2` | `HPM_MCAN2` | `PD08` | `PD09` |
| `can3` | `HPM_MCAN3` | `PD15` | `PD14` |

给常规固件的实现提示：

- 当前的逻辑 CAN0 用的是 `clock_can0`、`IRQn_MCAN0`、`PC00` 作 TX、`PC01` 作 RX，
  且 RX 开了上拉与迟滞。
- 将来若要放出全部四路总线：把 `clock_can1` 到 `clock_can3` 加进 core1 的时钟组，
  使用 `IRQn_MCAN1` 到 `IRQn_MCAN3`，为每路分配**独立的 MCAN message RAM 分片**，
  并把引脚配成 `MCANx_TXD` / `MCANx_RXD`，RX 同样开上拉与迟滞。
- **后续做 GPIO/LED 探测时要把这些引脚排除在外**，以免误伤。

## 板卡恢复工作的剩余项

- 找出那 17 个 LED GPIO。
- 确认 USB HS 与 JTAG 是否就是目前假定的 EVK 兼容引脚。
- 对照实际的 PHY / 网络变压器走线，确认 EtherCAT 端口 0/1 的 MII 引脚。
- 单独找出 Realtek 千兆以太网 PHY 的接口引脚。
