# HPM6E8Y 以太网引脚逆向

> **文档类型**：过程记录（硬件逆向）+ 硬件参考（引脚表）
> **适用范围**：`firmware/rmcs_board/boards/hpm6e8y/`，无原理图的 BGA196 板
> **状态**：现行有效（最后更新 2026-07-11；EtherCAT 侧已收口，Realtek 侧仅剩真实 PHY 地址未定）
> **相关文档**：[README.md](README.md)（板级说明） · [ECAT_BRIDGE_BRINGUP_NOTES.md](ECAT_BRIDGE_BRINGUP_NOTES.md)（bring-up 踩坑） · [GPIO_LED_REVERSE_ENGINEERING.md](GPIO_LED_REVERSE_ENGINEERING.md) · [CAN_PIN_REVERSE_ENGINEERING.md](CAN_PIN_REVERSE_ENGINEERING.md)

## 摘要

日期：2026-07-10（2026-07-11 补入最终端口映射）。

本文件记录这块**缺原理图**的板子在以太网引脚上的恢复进度。板上一共有**三个 RJ45**：
两个 EtherCAT（用片内 100M PHY）+ 一个 Realtek 千兆口，它们是三套独立的问题。

**最新的关键更正**：HPM6E\*Y\* 这颗芯片自带**两个片内 100M 以太网 PHY** 供 EtherCAT 使用。
`PA25` 和 `PA28` **不是**板上的 GPIO 黄色 LED，而是片内 PHY 的 LED / 地址 strap 引脚，
必须留给 PHY。

### 已收口的结论速查

| 项目 | 结论 |
|---|---|
| EtherCAT PHY 类型 | 片内两颗 JL1111 级 100M PHY，ID `0x937c4024` |
| EtherCAT 管理通路 | `PA30`/`PA31` 复用为 **ENET0 SMI** |
| **丝印 OUT / EtherCAT1 才是接主站的口** | 见[最终映射](#最终丝印--phy--esc-数据端口2026-07-11-状态探测) |
| Realtek 复位脚 | **`PE01`**，低电平有效 PHYRSTB |
| Realtek 管理脚 | `PF00` MDC / `PF01` MDIO，ID `0x001cc916` |
| Realtek RGMII 数据总线 | **PF 组（`PF02..PF15`）**，已用实发数据包证明 |
| 仍未定 | Realtek 的**真实** PHY 地址（`0` 和 `1` 都有响应） |

## 已确认的 EtherCAT 事实

对这两颗片内 100M PHY 而言，**封装外部引脚是模拟/strap 引脚，不是并行 MII 引脚**：

- PHY1：`PA16/PA17` 是 RD-/RD+，`PA18/PA19` 是 TD-/TD+，`PA24` 是 RBIAS，
  `PA25/PA26` 是 LED/地址 strap。
- PHY0：`PA20/PA21` 是 RD-/RD+，`PA22/PA23` 是 TD-/TD+，`PA27` 是 RBIAS，
  `PA28/PA29` 是 LED/地址 strap。
- 共享的管理引脚：`PA30` 是 MDIO，`PA31` 是 MDC。

片内 PHY 的数字侧使用**内部引脚**：

- PHY0 的 MII 接到 ESC0 P0：`PV00`..`PV11`、`PV15`。
- PHY1 的 MII 接到 ESC0 P1：`PW00`..`PW11`、`PW15`。
- 内部 PHY 复位输入：`PV12` 和 `PW12`，**低电平有效**，以 GPIO 方式驱动。
- 内部 PHY 的 25 MHz 参考时钟：`PW20` 和 `PW21`，功能为 `ESC0_REFCK`。

探测固件必须**按顺序**做这几件事：先使能 EtherCAT 时钟，驱动参考时钟，把差分/RBIAS/
LED0/LDO 引脚置为模拟模式，把 `PA25`/`PA28` 路由为 `ESC0_CTR_0`/`ESC0_CTR_1` 并给
LED1/地址 strap 引脚加上拉，最后释放 `PV12`/`PW12`。

完成上述 bring-up 之后，可用的管理通路是：

```text
PA30/PA31 引脚复用：ENET0 SMI
PHY ID：            0x937c4024
PHY 类型：           JL1111 级 100M PHY
```

同样这两个引脚上的 **ESC MII 管理通路**和 **TSW 逐端口 MDIO 通路都没能**读出有用的
PHY-ID / 链路结果。对这块板，请保持片内 PHY 的引脚复用不变，并使用 `ENET0` SMI 做
PHY 管理探测。

常规固件现在在 `board.c`/`board.h` 里遵循这个模型：

- 对 `hpm6e8y` 不再定义 `BOARD_ECAT_DISABLE_ESC_BRINGUP`。
- `PA16..PA29` **不**用作外部 MII 数据引脚。
- `PV00..PV11/PV15` 在内部把 ESC0 P0 接到 PHY0。
- `PW00..PW11/PW15` 在内部把 ESC0 P1 接到 PHY1。
- `PV12` 和 `PW12` 是低电平有效的 PHY 复位 GPIO。
- `PA25` 和 `PA28` 保持为 PHY 的 LED/地址 strap 引脚，同时也是 ESC 的链路源控制：
  `CTR_0` 对应 port1/PHY1，`CTR_1` 对应 port0/PHY0。
- `BOARD_ECAT_PORT0_PHY_ADDR = 2`，`BOARD_ECAT_PORT1_PHY_ADDR = 1`，与实测的 IN/OUT
  链路测试一致。

## 已确认的 RJ45 映射

链路测试采用**一次只插一根线**的方式，观察 CAN 帧 `0x5a0`、`0x5a1`、`0x5a2` 里的 BMSR 值。

| 物理接口 | MDIO 结果 |
| --- | --- |
| EtherCAT IN / EtherCAT0 | PHY 地址 `2`，连接时 BMSR 为 `0x786d` |
| EtherCAT OUT / EtherCAT1 | PHY 地址 `1`，连接时 BMSR 为 `0x786d` |
| Ethernet / Realtek 口 | PHY 地址 `0`、`1`、`2` 上都没有链路变化 |

最新的操作者抓包：

| 测试 | 通过的帧 | 结果 |
| --- | --- | --- |
| 插入 EtherCAT IN / EtherCAT0 线 | `0x5a2 ... 78 6d` | 确认 PHY 地址 `2` 与 IN 接口的对应 |
| 插入 EtherCAT OUT / EtherCAT1 线 | `0x5a1 ... 78 6d` | 确认 PHY 地址 `1` 与 OUT 接口的对应 |
| 插入 Realtek 以太网线 | `0x5e0`/`0x5e1 ... 79 ad`，`0x560`/`0x561 ... 00 1c c9 16` | 确认 Realtek 的管理通路、PHY 供电/复位/时钟，以及外部 RJ45 链路 |

**不插任何线时**，三个上报地址的 BMSR 都是 `0x7849`。插上 Ethernet/Realtek 那根线之后，
三个**仍然**停在 `0x7849`。因此**第三个 RJ45 不属于已发现的那些 JL1111 EtherCAT PHY 地址**。

PHY 地址 `0` 同样返回了 `0x937c4024`，但它并不随第三个 RJ45 建立链路。在做过"关闭广播
响应"的测试证明之前，请把它当作 JL1111 的广播/别名响应。**不要**把它映射到 Realtek
以太网口。

## 最终：丝印 <-> PHY <-> ESC 数据端口（2026-07-11，状态探测）

用 `debug-ecat-status-probe` 镜像确认，一次插一根线，同时读取逐端口的 PHY BMSR
（CAN `0x783`/`0x784`）和 ESC 的 DL Status 寄存器 `0x0110`（CAN `0x780`，低字节 bit4 =
ESC port0 物理链路，bit5 = ESC port1）。**这是端到端的权威映射**，并且与"PC 主站只有插在
OUT 口上才能枚举"这一功能测试互相印证。

| 丝印 | PHY MDIO 地址 | 探测帧 | ESC 数据端口 | EtherCAT 角色 | 连向 |
| --- | --- | --- | --- | --- | --- |
| EtherCAT0 / IN | 2 | `0x783`（固件称之为 "port0"） | ESC port1 | 下游 | 下一个从站 |
| EtherCAT1 / OUT | 1 | `0x784`（固件称之为 "port1"） | ESC port0 | 上游 | PC 主站 |

原始证据：

| 接线 | `0x783`（地址 2）BMSR | `0x784`（地址 1）BMSR | `0x780` DL Status | 有链路的 ESC 端口 |
| --- | --- | --- | --- | --- |
| EtherCAT0 / IN | `0x786d` 已连接 | `0x7849` 无 | `0x5923`（bit5 置位） | ESC port1 |
| EtherCAT1 / OUT | `0x7849` 无 | `0x786d` 已连接 | `0x5613`（bit4 置位） | ESC port0 |

**必须记住的三条后果**：

- **丝印的 IN/OUT 与 ESC 数据通路是反的**：面向主站的那个口（ESC port0，PC 网线必须插
  这里）是标着 **OUT / EtherCAT1** 的那个。**把主站插到 OUT 口上。**
- 固件宏 `BOARD_ECAT_PORT0_PHY_ADDR=2` / `BOARD_ECAT_PORT1_PHY_ADDR=1` 只是 **MDIO 轮询
  用的标签**，它们相对 ESC 数据端口是**反的**（固件的 "port0"/地址 2 物理上是 ESC
  port1）。这个反转正是为什么必须有 `BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT=1`
  （地址 1 的链路 -> ESC port0 的 NMII_LINK，地址 2 的链路 -> ESC port1）。
  **不要在不同时去掉 swap 的情况下去"纠正"这些标签**，否则链路检测会坏掉，主站将无法枚举。
- 想把主站入口挪到 IN 接口，属于 **PCB 走线改动**（要交换哪个 RJ45 的差分对接到哪颗片内
  PHY 的模拟引脚）；**没有任何 `PHY_CFG0`/ESC 寄存器**能重新指定哪颗 PHY 喂给哪个 ESC
  数据端口。

## Realtek 复位脚已确认：PE01（低电平有效 PHYRSTB）

外置的 Realtek RTL8211F **默认被按在复位状态**。`PE01` 就是它的低有效 `PHYRSTB`：
保持复位时，该 PHY 在 MDIO 上读出来全是 `0xffff`，且永远不建立链路（RJ45 灯全灭）；
把 `PE01` 驱动为高就释放了它，随即在 MDIO 地址 `0` 上立刻返回 `0x001cc916`。

这是由 `realtek_reset_scanner` 这个 core1 镜像（构建预设 `debug-realtek-reset-scanner`）
找到的——它把每个候选 GPIO 先拉低再拉高，然后重读 ENET0 MDIO 直到 Realtek 的 OUI 出现，
最终锁定在 `PE01`（CAN `0x7f0`，负载 `46 45 01 00 00 1c c9 16`）。

> **一个曾经的误判**：早先 probe-v8 那次抓包在没有任何固件释放 `PE01` 的情况下也看到了
> Realtek——那其实是读到了上一次启动遗留的状态。**在干净的冷启动下没有东西驱动 `PE01`，
> 所以 PHY 是不工作的。**

RTL8211F 用的是它自己的 25 MHz 晶振，**不需要 SoC 提供参考时钟**——只要释放复位就能起来。

**固件必须在任何 ENET0 MDIO/RGMII 操作之前先给 `PE01` 打一个脉冲**（先拉低、再释放为高、
等待约 160 ms 稳定）。`enet_packet_tester` 现在在 `release_realtek_reset()` 里就是这么做的。

注意：core0 的 ESC bring-up（`board_init_ethercat` + `ecat_hardware_init`）**不影响**
Realtek——把它关掉并不能让 PHY 活过来。那条路径只是在 ENET 探测类诊断镜像里被禁用以
保持镜像精简，**并不是因为它和 Realtek 冲突**。

## Realtek 以太网的排查计划

外置的 Realtek 以太网 PHY 是一个独立目标。**不要**拿 EtherCAT 那边 JL1111 的结论去套它。

探测固件 v8 确认了 Realtek 的管理引脚：

```text
PF00 = ETH0_MDC
PF01 = ETH0_MDIO
访问路径 = ENET0 SMI
PHY ID = 0x001cc916
观察到有响应的地址 = 0 和 1
连接时的 BMSR = 0x79ad
```

`0x001cc916` 是 Realtek RTL8211 级千兆 PHY 的 ID。`0x79ad` 里链路状态和自协商完成位都
置了，所以外部以太网 RJ45、Realtek PHY 的供电、复位、时钟以及 MDIO/MDC 通路**都是通的**。

在第一次成功的扫描里，地址 `0` 和 `1` 返回了相同的 ID 和链路状态。在后续测试隔离出唯一
真实地址之前，请把它当作**地址别名/广播/strap 的歧义**。就引脚恢复而言，说管理引脚是
`PF00/PF01` 是安全的；最终固件应当在弄清 Realtek 特有的初始化流程之后，选择那个持续有
响应的地址。

确定它的方法：

1. 读 Realtek 芯片封装上的丝印型号，然后使用**那一份确切的** Realtek 数据手册引脚表。
   需要的引脚有：`MDC`、`MDIO`、复位、时钟输入或晶振引脚、PHY 地址 strap、RGMII/RMII
   模式 strap，以及 RGMII/RMII 的 MAC 侧引脚。
2. 在 HPM6E8Y 的 BGA196 球脚表里，把所有**没有键合出来**的 RGMII/RMII 或 MDIO/MDC
   候选 bank 划掉。
3. MDIO/MDC 引脚现已确定，因此剩下的数据引脚问题只是：Realtek 的 RGMII 数据总线用的是
   连续的 `PF02..PF13` 组，还是 `PB00..PB11` 组。

探测固件版本 `8` 加入了这个首要的、面向 Realtek 的候选：

```text
bus 3：PF00/PF01 作为 ETH0_MDC/ETH0_MDIO，经 ENET0 SMI 读取
状态帧：   0x733
基础帧：   0x5e0 + phy_addr
PHY ID 帧：0x560 + phy_addr
命中帧：   0x660 + phy_addr
```

优先检查的高价值 MDIO/MDC 候选：

| 候选 | 理由 |
| --- | --- |
| `PF00/PF01` | 已确认为 Realtek 的 `ETH0_MDC`/`ETH0_MDIO`；在探测 v8 里作为 bus `3` 扫描 |
| `PA30/PA31` | 已证明在 EtherCAT 管理上是活的，但当前扫描中 Realtek 并未在这里出现 |
| 任何与 BGA196 表中已键合 RGMII bank 绑定的 MDIO/MDC 引脚对 | 最终选择必须遵循实际的键合球脚表和 Realtek 封装引脚表 |

HPM6E80 引脚复用表里剩下的、不冲突的 ETH0/RGMII 候选：

| 候选 bank | 信号 | 冲突情况 |
| --- | --- | --- |
| `PF00..PF15` | `MDC`、`MDIO`、`TXCK`、`TXD0..3`、`TXEN`、`RXDV`、`RXD0..3`、`RXCK`、`RXER`、`TXER` | **最强候选**，因为 `PF00/PF01` 已确认是 Realtek 管理引脚，而其余部分是一个连续的已键合 ETH0 bank |
| `PB00..PB11` 加 `PF00/PF01` | `RXDV`、`RXD0..3`、`RXCK`、`TXCK`、`TXD0..3`、`TXEN`，管理仍在 `PF00/PF01` | 仍有可能；HPM 的例程确实会把管理和数据分在不同 bank |
| `PB00..PB11` 加 `PA30/PA31` | 同样的数据引脚，外加 `MDIO/MDC` | 不太可能；`PA30/PA31` 已经在读 EtherCAT 的 JL1111，且插上以太网 RJ45 时并未暴露出 Realtek |
| `PA16..PA31` / 混合 PA bank | 确实存在 RGMII/MII 的备用功能 | **Realtek 请避开这里**；这些引脚已确认是 EtherCAT 片内 PHY 的模拟/strap/管理引脚 |

至于数据引脚，要找的是一整个**连接到 Realtek 封装的、已键合的完整 RGMII/RMII bank**。
千兆 PHY 通常需要 RGMII，所以应当预期看到成组的 `TXC`、`TXEN/CTL`、`TXD0..3`、`RXC`、
`RXDV/CTL`、`RXD0..3`，再加上管理与复位。

## Realtek RGMII 数据总线已确认：PF 组（PF02..PF15）

由 ENET 数据包测试固件证明：在 `PE01` 已释放的前提下，板子发出的 RGMII 帧被 PC 完整
收到（源 MAC `02:52:4d:43:53:03` = 候选 0 / PF 组，变体 3 = 1000M、TX+RX 延迟 7/7；
负载 `RMCS-HPM6E8Y-ENET-PIN-TEST`，EtherType `0x88b5`）。**线上出现一个格式正确的帧，
就证明 PF 组的 RGMII 发送引脚映射与时序都是对的。** 最终映射：

```text
PF00 MDC   PF01 MDIO
PF02 TXCK  PF03..PF06 TXD0..3  PF07 TXEN
PF08 RXDV  PF09..PF12 RXD0..3  PF13 RXCK
PF14 RXER  PF15 TXER（RTL8211F 的 RGMII 不使用 ER；保留无害）
PE01 PHYRSTB（低有效复位，驱动为高即释放）
```

可工作的线速率为 1000M，`tx_delay=7`、`rx_delay=7`（数据包测试固件的变体 3）。

**仍未解决的部分**：

- Realtek 数据总线：**已解决** —— PF 组（见上）。PB 那个备选已被排除。
- Realtek 的真实 PHY 地址：第一次成功扫描在地址 `0` 和 `1` 上看到了相同的 `0x001cc916`
  响应；在隔离出广播/strap 行为之前，两者都存疑。
- Realtek 复位脚：**已解决** —— `PE01`，低有效 PHYRSTB（见上文已确认一节）。时钟来自
  PHY 自己的 25 MHz 晶振，不需要 SoC 时钟。

## 剩余的收口测试

EtherCAT 的引脚恢复在 MDIO/链路层面**已经完成**。唯一还有价值的 EtherCAT 检查是一次
"一次插一根线"的对照测试：

| 接线状态 | 预期的 EtherCAT 结果 |
| --- | --- |
| 不插 EtherCAT 线 | `0x5a1 ... 78 49` 且 `0x5a2 ... 78 49` |
| 只插 EtherCAT IN | `0x5a2 ... 78 6d`；`0x5a1` 保持 `78 49` |
| 只插 EtherCAT OUT | `0x5a1 ... 78 6d`；`0x5a2` 保持 `78 49` |

**Realtek 的 RGMII 数据引脚无法用 MDIO 证明**。MDIO 只能证明管理引脚和 PHY 侧的链路。
要给 Realtek 数据总线收口，就得跑 ENET0 数据包测试：先试下面第一个候选，只有在数据包
过不去时才试第二个：

| 候选 | 引脚映射 |
| --- | --- |
| `PF00..PF15` | `PF00` MDC，`PF01` MDIO，`PF02` TXCK，`PF03..PF06` TXD0..3，`PF07` TXEN，`PF08` RXDV，`PF09..PF12` RXD0..3，`PF13` RXCK，`PF14` RXER，`PF15` TXER |
| `PB00..PB11` + `PF00/PF01` | `PB00` RXDV，`PB01..PB04` RXD0..3，`PB05` RXCK，`PB06` TXCK，`PB07..PB10` TXD0..3，`PB11` TXEN，管理仍在 `PF00/PF01` |

**数据包测试通过的标准**是：PC 能从 Realtek RJ45 上看到来自 ENET0 的、EtherType 为
`0x88b5` 的裸帧；最好板子那边也能通过 CAN 上报匹配的接收计数。在这样的数据包测试通过
之前，Realtek 数据总线只能记作"`PF02..PF15` 很可能，但未最终确定"。

## ENET 数据包测试固件

这个测试固件是外置 Realtek 以太网口在**数据面**上的收口手段。它不改动常规的 EtherCAT
应用，而是构建一个临时的 core1 镜像，反复尝试仅有的两个有意义的 RGMII 引脚 bank：

| 候选编号 | 候选 | 源 MAC 后缀 |
| --- | --- | --- |
| `0` | `PF00..PF15` | `02:52:4d:43:53:00`..`0b` |
| `1` | `PB00..PB11` 加 `PF00/PF01` 作管理 | `02:52:4d:43:53:10`..`1b` |

对每个候选，它还会尝试四种常见的 HPM RGMII 延迟预设和三种 MAC 线速率。**这一点很重要**：
观察到的这颗 Realtek 类 PHY 在 BMSR 里报告了链路（`0x79ad`），却在第一版测试固件所用的
RTL8211 PHYSR 寄存器上返回 `0x0000`，所以测试固件**不能依赖 PHYSR 来选择速率**。

| 变体编号 | TX 延迟 | RX 延迟 | MAC 速率 |
| --- | --- | --- | --- |
| `0` | `0` | `0` | 1000M |
| `1` | `0` | `7` | 1000M |
| `2` | `7` | `0` | 1000M |
| `3` | `7` | `7` | 1000M |
| `4` | `0` | `0` | 100M |
| `5` | `0` | `7` | 100M |
| `6` | `7` | `0` | 100M |
| `7` | `7` | `7` | 100M |
| `8` | `0` | `0` | 10M |
| `9` | `0` | `7` | 10M |
| `10` | `7` | `0` | 10M |
| `11` | `7` | `7` | 10M |

也就是说，实用的第一轮搜索是 `2 × 12 = 24` 个变体，**而不是**逐引脚的随机排列。如果某个
变体初始化成功且数据包能通过，RGMII bank 就定下来了。如果某个 bank 能初始化但数据包不
稳定，后续可以只做时序扫描，把延迟搜索扩展到全部 `8 × 8` 个取值，而不改动引脚映射。

测试固件版本 `3` 把 MDIO/链路状态只当作**参考信息**。即便改了引脚复用之后 Realtek 的
PHY ID 读取失败，它仍然会按选定的速率/延迟变体强制配置 ENET0 并发送裸帧。只有 ENET 的
DMA/控制器初始化失败才会阻止该变体发送。

> ⚠️ **重要（已修复的回归）**：Realtek PHY **只在 MDIO 扫描器所建立的那套"片内 PHY 已
> 激活"的环境下才会建立链路。** 早先某版测试固件只使能了 `clock_gpio/can0/eth0` 并复用了
> RGMII 引脚，**遗漏了** ESC 核心/PHY 时钟、`PW20`/`PW21` 的 `ESC0_REFCK` 参考时钟，
> 以及 `PV12`/`PW12` 的复位释放。在那种状态下外置 Realtek 得不到时钟——没有链路、RJ45
> 灯全灭。现在测试固件会在候选循环之前先调用同一套 `release_internal_phys()` bring-up
> （ESC/TSN 时钟 + `esc_core_enable_clock`/`esc_phy_enable_clock` + 参考时钟 + 复位释放），
> 与 MDIO 探测 v8 读到 `0x001cc916`、链路 BMSR `0x79ad` 时的环境一致。
> **如果 RJ45 灯是灭的，先确认这段 bring-up 有没有跑。**

构建数据包测试固件：

```bash
cmake --preset debug-enet-packet-tester -S firmware/rmcs_board/ecat \
    -DBOARD=hpm6e8y
# 下面的工具链路径是 [前机路径]，换机器后改成自己的安装位置
GNURISCV_TOOLCHAIN_PATH=/home/zyi/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux \
    cmake --build firmware/rmcs_board/ecat/build
```

经 DFU 烧录：

```bash
dfu-util -d a11c:a904 -e
dfu-util -d a11c:a904 -a 0 \
    -D firmware/rmcs_board/ecat/build/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu \
    -R
```

把 PC 接到 Realtek 以太网 RJ45 上并把 PC 的网口 up 起来。这个裸帧测试**不需要 IP 地址**。

在 PC 上观察板子发出的裸以太网帧：

```bash
sudo ip link set dev <iface> up
sudo tcpdump -i <iface> -e -XX 'ether proto 0x88b5'
```

板子发出的帧使用这些源 MAC：

| 源 MAC 模式 | 含义 |
| --- | --- |
| `02:52:4d:43:53:00`..`0b` | PF 候选，变体编号 `0`..`11` |
| `02:52:4d:43:53:10`..`1b` | PB 候选，变体编号 `0`..`11` |

在测试固件轮换变体期间，从 PC 持续向板子回发裸帧：

```bash
sudo python3 - <<'PY'
from scapy.all import Ether, Raw, sendp

iface = "<iface>"
frame = Ether(dst="ff:ff:ff:ff:ff:ff", type=0x88B5) / Raw(b"pc-to-hpm6e8y")
sendp(frame, iface=iface, count=400, inter=0.05, verbose=False)
PY
```

测试固件通过 CAN0（`PC00`/`PC01`）以 1 Mbps 经典 CAN 上报。

身份帧：

| CAN ID | 负载 |
| --- | --- |
| `0x740` | `45 4e 45 54 03 02 0c 52` |

状态帧：

| CAN ID | 字节 0 | 字节 1 | 字节 2 | 字节 3 | 字节 4 | 字节 5 | 字节 6 | 字节 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x741` | `0x45`（`E`） | 候选 | 变体编号 | 阶段 | 状态 | PHY 地址 | 选定速率 | 双工/链路 |

**阶段**：`P` = 引脚已配置，`I` = ENET 初始化结果，`T` = 该变体正在收发。
**状态**：`0` 正常，`1` 没有 Realtek PHY，`2` 无链路，`3` 速率/双工未确定，
`4` ENET DMA/控制器初始化失败。在测试固件版本 `3` 中，状态 `1`、`2`、`3` **不会**阻止
发包，只有状态 `4` 会。
**选定速率**：`1` = 10M，`2` = 100M，`3` = 1000M。字节 7 的 bit 0 是链路，bit 1 是全双工。

计数帧：

| CAN ID | 字节 0 | 字节 1 | 字节 2 | 字节 3 | 字节 4 | 字节 5 | 字节 6 | 字节 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x750` | `0x43`（`C`） | 候选 | 变体编号 | 发送成功数低字节 | 接收匹配数低字节 | 发送失败数低字节 | 接收总数低字节 | 链路 |

PHY 帧：

| CAN ID | 含义 |
| --- | --- |
| `0x760 + phy_addr` | Realtek PHY-ID 命中，负载字节 4..7 是 ID1/ID2 |
| `0x770` | 选定 PHY 的 BMSR/PHYSR 以及解码出的速率/链路 |

**结果解读**：

- 如果 `tcpdump` 看到 `02:52:4d:43:53:00`..`0b`，说明 Realtek 数据 bank 是 `PF`。
- 如果 `tcpdump` 看到 `02:52:4d:43:53:10`..`1b`，说明 Realtek 数据 bank 是 `PB`。
- 如果在 Scapy 发送方运行期间 CAN `0x750` 的字节 4 在增长，说明该候选/延迟组合下板子的
  **接收**方向也是通的。
- 如果状态是 `4`，说明选定的引脚 bank 很可能没有把 `RXCK` 喂给 ENET0，那这个 bank 大概
  率是错的。

## 历史假设：外置 TSW 方案

早期笔记曾探讨过纯外置 `TSW0_P1`/`TSW0_P3` 的解释。**把它保留为将来调查任何独立外置千兆
PHY 时的备选思路，但不要用它去初始化 HPM6E\*Y\* 封装里那两颗 EtherCAT 100M PHY。**
对 EtherCAT 的 PHY-ID bring-up 而言，上文那套片内 PHY 路径才是主模型。

## MDIO 探测固件

构建探测镜像：

```bash
cmake --preset debug-mdio-pin-scanner -S firmware/rmcs_board/ecat \
    -DBOARD=hpm6e8y
cmake --build firmware/rmcs_board/ecat/build
```

烧录生成的 DFU 镜像：

```bash
dfu-util -d a11c:a904 -e
dfu-util -d a11c:a904 -a 0 \
    -D firmware/rmcs_board/ecat/build/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu \
    -R
```

探测程序跑在 core1 上，通过 CAN0（`MCAN0`，`PC00`/`PC01`）以 1 Mbps 经典 CAN 上报。
它保持片内 PHY 的引脚复用不变，并反复用四种管理通路变体扫描 PHY 地址 `0..31`：

- bus `0`：片内 PHY bring-up，外加 `PA30`/`PA31` 上的 ESC MII 管理。
- bus `1`：`PA30`/`PA31` 上的 `ENET0` SMI 管理，片内 PHY 引脚保持不变。
- bus `2`：`PA30`/`PA31` 上的 `TSW0_P1` MDIO 管理，片内 PHY 引脚保持不变。
- bus `3`：`PF00`/`PF01` 上的 `ENET0` SMI 管理，片内 PHY 引脚保持不变。

每一轮扫描以一个身份帧开始：

| CAN ID | 负载 |
| --- | --- |
| `0x72f` | `4d 44 49 4f 08 04 52 53` |

它解码为 `MDIO`、探测版本 `8`、bus 数量 `4`。**如果这一帧没有出现，说明新的探测固件没在
跑，或者 CAN 抓包的过滤条件把它挡掉了。**

状态帧：

| CAN ID | 字节 0 | 字节 1 | 字节 2 | 字节 3 | 字节 4 | 字节 5 | 字节 6 | 字节 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x730 + bus` | `0x4D`（`M`） | bus | 物理端口 | 阶段 | 命中数 | 错误数 | 复位驱动 | 访问方式 |

**阶段**：`S` = 扫描开始，`D` = 扫描完成，`E` = 有 MDIO 事务超时。
**访问方式**：`C` = ESC MII 管理通路，`T` = TSW 逐端口 MDIO 引擎，`E` = `ENET0` SMI 引擎。

原始读取帧：

| CAN ID | 字节 0 | 字节 1 | 字节 2 | 字节 3 | 字节 4..5 | 字节 6..7 |
| --- | --- | --- | --- | --- | --- | --- |
| `0x500 + bus * 32 + phy_addr` | `0x52`（`R`） | bus | PHY 地址 | 读取成功 | 寄存器 2 / PHYID1 | 寄存器 3 / PHYID2 |
| `0x580 + bus * 32 + phy_addr` | `0x52`（`R`） | bus | PHY 地址 | 读取成功 | 寄存器 0 / BMCR | 寄存器 1 / BMSR |

PHY 命中帧：

| CAN ID | 字节 0 | 字节 1 | 字节 2 | 字节 3 | 字节 4..5 | 字节 6..7 |
| --- | --- | --- | --- | --- | --- | --- |
| `0x600 + bus * 32 + phy_addr` | `0x4D`（`M`） | bus | 物理端口 | PHY 地址 | 寄存器 2 / PHYID1 | 寄存器 3 / PHYID2 |

PHY 身份的解码方式：

```text
phy_id = (reg2 << 16) | reg3
oui = (reg2 << 6) | ((reg3 >> 10) & 0x3f)
model = (reg3 >> 4) & 0x3f
revision = reg3 & 0x0f
```

Realtek 的千兆 PHY 通常显示为 `0x001c****` 区间的 ID。这块板上通过 bus `1`
（`PA30`/`PA31` 上的 `ENET0` SMI）观察到的 HPM 片内 100M PHY ID 是 `0x937c4024`。

**如果所有 bus 都只返回 `0xffff`/`0x0000`**，那嫌疑就要从引脚复用转向硬件 bring-up：
VIO_B01 的 3.3 V、`VDD_PHY0CAP`/`VDD_PHY1CAP` 的 1.2 V LDO 电容、RBIAS 电阻、
`PA25/PA26/PA28/PA29` 上的 strap 电平，或者某个缺失/被挡住的片内 PHY 复位/时钟要求。
