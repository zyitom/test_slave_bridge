# HPM6E8Y EtherCAT 桥 bring-up 笔记

> **文档类型**：过程记录（bring-up 踩坑 + 实测数据）
> **适用范围**：`firmware/rmcs_board/boards/hpm6e8y/`，EtherCAT 桥固件
> **状态**：现行有效（最后更新 2026-07-13）
> **相关文档**：[README.md](README.md)（板级说明） · [ETHERNET_PIN_REVERSE_ENGINEERING.md](ETHERNET_PIN_REVERSE_ENGINEERING.md)（网络引脚逆向） · [GPIO_LED_REVERSE_ENGINEERING.md](GPIO_LED_REVERSE_ENGINEERING.md) · [../../ecat/README.md](../../ecat/README.md)

## 摘要

本文件记录在**没有原理图**的情况下把 `hpm6e8y` EtherCAT 桥在硬件上跑起来时踩到的坑。
**修改这块板的移植代码时请把本文放在手边**：其中好几条事实与调查之初沿用的
"HPM6E00 EVK 式"假设是相反的。

**最容易踩的三个坑**：
[ESC 逻辑端口与物理标签是反的](#6-esc-逻辑端口映射与物理标签相反)（当初"找不到从站"的
真凶）、[HPM6E8Y 用的是片内 EtherCAT PHY](#1-hpm6e8y-使用片内-ethercat-phy)、
[第三个 RJ45 不是 EtherCAT](#4-第三个-rj45-不是-ethercat)。

现象与处置的速查见文末[症状对照表](#症状对照表)。

## 当前状态

干净的常规固件已经能稳定跑起 EtherCAT 桥：

```text
EtherCAT link up on "enxc84d4429a4d3": slave "ECAT_Device", 48B chunks, expected WKC 3
EtherCAT bridge connected on enxc84d4429a4d3, session established.
EtherCAT cycle rate: 12.1-12.2 kHz (0 wkc errors)
EtherCAT link closing: 364428 cycles total, 0 wkc errors
```

这说明 EtherCAT 物理链路、ESC、PDO 映射、WKC 以及协议会话全部工作正常。

### 2026-07-13 IgH 丢包与延迟验证

有两个**主机侧协议 bug** 曾经看起来像是传输层或双核丢数据：

1. CAN 反序列化器暴露出的负载 span 背后是它的待处理字节缓存，而它在随后读取硬件时间戳
   时又复用了那块缓存。当负载和时间戳恰好跨越输入分块边界时，**时间戳会覆盖掉负载的前
   四个字节**。抓到的坏数据解出来正好是板子的上电微秒数。现在负载和时间戳改为用一次
   连续的 `peek_bytes()` 窗口读取。
2. 主机把**每一次 libusb 接收完成都当成一个完整的协议数据报**，于是在该边界上被切开的
   字段被直接丢弃。而 USB bulk 完成事件其实是一条可靠字节流的任意切片，在 EtherCAT 桥
   的跨核环形缓冲接力下尤其如此。现在 USB 和 EtherCAT 的接收路径都会**跨回调保留反
   序列化器的中间状态**。

另外，USB 发送池在 64 个异步传输全部占用时会施加**背压**，不再返回空缓冲、在高负载下
静默丢掉一个包。

硬件实测结果（CAN0 接 CAN1、CAN2 接 CAN3，CAN-FD 仲裁 1 Mbit/s、数据段 5 Mbit/s）：

```text
IgH，每流 12k 帧/秒，30 秒：
  每条流 352907 发送 / 352907 接收
  gap=0 corrupt=0 reorder=0
  1122973 个 EtherCAT 周期，0 WKC 错误

IgH，每流目标 16k 帧/秒，20 秒：
  每条流 312145 发送 / 312145 接收（实际约 15.6k）
  gap=0 corrupt=0 reorder=0
  739822 个 EtherCAT 周期，0 WKC 错误

IgH + SCHED_FIFO，每流 16k 帧/秒，12 秒：
  每条流 192000 发送 / 192000 接收
  gap=0 corrupt=0 reorder=0，0 WKC 错误

IgH + SCHED_FIFO，每流 18k 帧/秒，12 秒：
  每条流 216003 发送 / 216003 接收
  gap=0 corrupt=0 reorder=0，0 WKC 错误

USB，每流目标 16k 帧/秒，20 秒：
  每条流 239359 发送 / 239359 接收（实际约 12.0k）
  gap=0 corrupt=0 reorder=0
  因非实时主机调度而跳过 80642 个发送时隙

USB + SCHED_FIFO，每流 18k 帧/秒，10 秒：
  每条流 180001 发送 / 180001 接收
  gap=0 corrupt=0 reorder=0

USB 与 IgH 均加 SCHED_FIFO，每流目标 20k 帧/秒：
  CAN0 在约 19.4k 帧/秒处见顶
  CAN2 在约 19.8k 帧/秒处见顶
  EtherCAT 仍报 0 WKC 错误，两种传输均报 0 数据损坏
```

`missed pacing slots`（跳过的发送时隙）指的是压力发生器因调度延迟或传输背压而**根本
没有提交**的帧。它们**不是**提交之后丢失的包；要判断后者请对比显式的发送/接收计数。

USB 和 IgH 在 20k 处几乎相同的平台期，说明**物理 CAN 发送路径**才是持续速率的上限。
20k 的命令周期是 50 us，而实测帧在 CAN0 上约占 51.5 us、在 CAN2 上约占 50.5 us
（CAN ID 与负载会影响位填充）。一旦 32 元素的非阻塞 MCAN 发送 FIFO 满了，固件就会报
`downlink_buffer_full` 并丢弃该命令；**EtherCAT 和 USB 都无法给 CAN 线增加容量**。

在实测的双流配置下，那 4 字节的接收时间戳**不是**持续速率瓶颈。一个标准 8 字节 CAN
上行字段，带时间戳是 15 字节、不带是 11 字节。两条 18k 流时，带时间戳的上行流量也只有
0.54 MB/s。去掉时间戳能减少 26.7% 的字节负载、增加 36.4% 的字节余量，但**并不能抬高
那个约 19.4-19.8k 的物理 CAN 天花板**。

**不要把这个结论原样推广到四条同步总线的情形。** 四个带时间戳的字段占 60 字节，因而
会跨越两个 44 字节的 ARQ 负载块；四个不带时间戳的字段正好占 44 字节，一块装得下。在
四条 18k 帧/秒的流下，字节速率带时间戳是 1.08 MB/s、不带是 0.792 MB/s。前者相对下文实测
的 IgH 全双工字节回显速率**几乎没有余量**。所以：时间戳不是当前双总线丢数据的原因，
但如果四条总线都要同时跑到接近 18k，**去掉时间戳或把它做成可选项是有用的**。注意 PDO
的毛预算（`44 字节 × 约 38 kHz = 约 1.67 MB/s`）**不等于**应用层持续流吞吐。

有一次 18k 的运行在某个五秒周期率日志点附近出现了 21-22 个上行字段缺失，但重新跑的
12 秒运行跨越两个日志点、每条流都是 `216003/216003`。该瞬态**未能复现**，因此不能把
"日志打印"确认为根因。不过话说回来，**同步的格式化与输出本来就不该放在生产环境的实时
周期线程里**；正确做法是发布计数器，由更低优先级的线程去打印。

无队列的 CAN0 到 CAN1 往返延迟（RTT），单帧在途、CAN-FD，传输线程在隔离的 CPU 7 上以
`SCHED_FIFO 80` 运行，发送方在 CPU 6 上以 `SCHED_FIFO 70` 运行：

```text
USB（50000 个样本）：p50 125.0 us，p99 152.2 us，p99.9 209.5 us，
  max 1111.1 us，0 次超时
IgH（50000 个样本）：p50 129.8 us，p99 140.6 us，p99.9 150.9 us，
  max 291.9 us，0 次超时，0 WKC 错误
```

> ⚠️ **发送方的 CPU 亲和性与 `SCHED_FIFO` 策略必须在板卡对象构造完成之后再设置。**
> 在构造之前设置，会让 SDK 那个 250 ms 的 keepalive 线程继承发送方的 CPU 和 FIFO 优先级。
> 于是忙等的发送方把该线程饿死，固件那 1000 ms 的会话租约在约 7.5k 个无队列帧之后过期，
> 之后所有 CAN 命令都被忽略——**而 EtherCAT 侧仍然是 0 WKC 错误**，极具迷惑性。
> 这是一个主机侧基准测试的调度 bug，不是 CAN 或 EtherCAT 丢数据。

实测 IgH 周期率约 37 kHz，即 27 us 的过程数据周期。完整的 CAN 环回 RTT 是若干个这样的
调度/传输阶段加上 CAN 线上时间；把 ESC/SSC 挪到 core1 或许能降低内部交接开销，但它**不是**
本次观察到的数据损坏或丢失的原因。

### 2026-07-13 纯传输层的 USB 与 IgH 对比

core1 的字节回显验证镜像可以把主机传输层、core0 接力、跨核环形缓冲、core1 交接这几段
从外部 CAN 接线中隔离出来单独测。两种模式使用了相同的主机工具与 CPU 布局：I/O 线程在
CPU 7 上跑 `SCHED_FIFO 80`，发送方钉在 CPU 6，延迟测试时保持单帧在途。

```text
44 字节帧（一个 ARQ 负载块）：
  USB，20 秒：233850/233850，损坏 0
    RTT p50 80.1 us，p99 110.9 us，p99.9 123.8 us，max 1019.7 us
  IgH，20 秒：252574/252574，损坏 0，0 WKC 错误
    RTT p50 78.2 us，p99 87.6 us，p99.9 109.6 us，max 135.3 us

64 字节帧（两个 ARQ 负载块）：
  USB，30 秒：350541/350541，损坏 0，RTT p50 80.1 us，p99 112.1 us
  IgH，30 秒：236916/236916，损坏 0，0 WKC 错误，
    RTT p50 131.8 us，p99 137.0 us

44 字节帧，64 帧在途，10 秒：
  USB：360819/360819，每方向 1550.4 KiB/s，损坏 0
  IgH：247928/247928，每方向 1065.3 KiB/s，损坏 0，0 WKC 错误
```

44 字节那组才是**单 PDO 的相关场景**：USB 和 IgH 的中位数基本相同，而 IgH 的尾部更收敛。
从 44 字节跨到 64 字节，IgH 的中位数增加约 54 us，因为这一帧需要第二个 ARQ 块；**USB
则不受 EtherCAT PDO 尺寸量化的影响**。这组纯传输层的结果还经受住了 USB -> IgH -> USB
的所有权切换，全程无复位、无字节丢失。

**从这些软件时间戳里得不到绝对的单向延迟数字**：主机的稳态时钟和板子的时钟并未同步。
在"下行与上行对称"这一显式假设下，44 字节 RTT 的一半给出单向中位数估计：USB 40.1 us、
IgH 39.1 us；p99 估计分别是 55.5 us 和 43.8 us。要拿到真值级别的单向延迟，请使用同步的
硬件时钟，或者示波器/逻辑分析仪。

USB 运行时的产品字符串是 `RMCS EtherCAT Bridge v<版本号>`，**不是** `RMCS Agent v<版本号>`。
主机扫描器现在只对 HPM6E8Y 的 PID `0xA904` 接受这个确切标识，同时仍然要求协议版本精确
匹配。USB 压力测试与延迟工具**不再使用** `dangerously_skip_version_checks`。恢复常规
core1 镜像之后，默认的 USB API 完成了真实的协议会话握手，从而确认跨核两个方向都是通的。

`ecat_board_test` 打印的 `done: 0 CAN frames, 0 UART bytes received` **并不意味着
EtherCAT 失败**。那个 example 会在 CAN0 和 UART0 上发流量，并统计板子**收回来**多少。
只有当外部 CAN 节点向板子发帧时，CAN 帧计数才会增长；只有当 UART 被环回或接了一个会
应答的设备时，UART 字节计数才会增长。

## 干净的常规固件

常规桥固件应当在**关闭所有 core1 诊断模式**的前提下构建：

```bash
# 下面的工具链路径是 [前机路径]，换机器后改成自己的安装位置
GNURISCV_TOOLCHAIN_PATH="$HOME/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux" \
cmake --build firmware/rmcs_board/ecat/build-ecat-normal-debug --target rmcs_ecat_core0
```

烧录：

```bash
sudo dfu-util -d 0xa11c:0xa904 -a 0 \
  -D firmware/rmcs_board/ecat/build-ecat-normal-debug/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

跑主机侧测试：

```bash
sudo ip link set dev enxc84d4429a4d3 up
sudo ./host/build/examples/ecat_board_test enxc84d4429a4d3 30
```

**健康的 EtherCAT 结果应该是**：发现一个名为 `ECAT_Device` 的从站，expected WKC 为 3，
整个运行期间 WKC 错误为零。

## 主要陷阱

### 1. HPM6E8Y 使用片内 EtherCAT PHY

**不要把 HPM6E00 EVK 的外部 MII 引脚假设照搬到这块板上。**

HPM6E\*Y\* 封装内含**两个片内 100M EtherCAT PHY**。外部的 PA 引脚大多是模拟/strap 引脚：

- PHY 的差分/RBIAS/strap 引脚集中在 `PA16..PA29` 附近。
- 共享的管理接口是 `PA30` 作 MDIO、`PA31` 作 MDC。
- ESC 的数字 MII 侧使用**内部**引脚：
  - `PV00..PV11/PV15`
  - `PW00..PW11/PW15`
- PHY 复位是内部 GPIO 引脚 `PV12` 和 `PW12`，**低电平有效**。
- `PW20` 和 `PW21` 提供内部 PHY 的参考时钟。

**不要**把 `PA16..PA29` 复用成外部 ESC MII 数据引脚。

### 2. PA25 和 PA28 不是普通的黄色 LED

`PA25` 和 `PA28` 最早是在 LED 探测时被发现的，但它们**实际上是片内 PHY 的 LED / 地址
strap 引脚**。请把它们当作与 PHY strap / 链路指示相关的引脚，**而不是可自由使用的 GPIO
状态灯**。

常规固件只会归位那些安全的 LED。EtherCAT 的 RUN/ERR 灯是通过 `ESC0_CTR_2` 和
`ESC0_CTR_3` 引到 `PC20` 和 `PC21` 的。

**红色 ERR 灯不亮本身不是故障**。在没有 AL 错误的健康 INIT/OP 流程中，错误灯本来就可能
是熄灭的。

### 3. ENET0 SMI 是已验证的片内 PHY 管理通路

读取那两颗 JL1111 级片内 PHY 的可靠方式是：

```text
PA30/PA31 复用为 ENET0 MDIO/MDC
访问路径：ENET0 SMI
PHY ID：  0x937c4024
```

固件会在读 PHY 时**临时**把 `PA30/PA31` 复用到 ENET0 SMI，读完再恢复成 ESC 的 MDIO/MDC。

### 4. 第三个 RJ45 不是 EtherCAT

Realtek RTL8211F 那个以太网口是独立的 RJ45，也是独立的一摊问题：

```text
PF00/PF01 = ENET0 MDC/MDIO
PE01      = RTL8211F PHYRSTB，低有效
PF02..PF15 = 已确认的 RGMII 数据总线
```

**不要用 Realtek 那个 RJ45 做 EtherCAT 测试。** EtherCAT 测试必须用那两个 JL1111 片内
PHY 的 RJ45。

### 5. 物理 RJ45 与 PHY 地址的对应关系

硬件上/面向用户的命名是准确的：标着 EtherCAT0 的 RJ45 就是物理的 EtherCAT **IN** 口，
标着 EtherCAT1 的 RJ45 就是物理的 EtherCAT **OUT** 口。

只接 EtherCAT0/IN 时的状态探测确认：

```text
0x783 = 50 42 00 03 31 00 78 6d  # port0 / PHY 地址 2：读取成功 + 链路已连接
0x784 = 50 42 01 01 31 00 78 49  # port1 / PHY 地址 1：读取成功 + 链路断开
0x782 = 47 50 00 00 10 10 03 00  # GPR_CFG2 = 0x10100000，交换后的链路映射
```

这确认了 EtherCAT0/IN 对应 PHY 地址 `2`。但它**并不能免除**固件把该物理链路交换到 ESC
逻辑端口 1 的要求，见下一条。

一次只插一根线时观察到的映射：

| 物理接口 | MDIO PHY 地址 | 连接时的 BMSR |
| --- | --- | --- |
| EtherCAT0 / 物理 IN 标签 | `2` | `0x786d` |
| EtherCAT1 / 物理 OUT 标签 | `1` | `0x786d` |

**不插线时读不到 BMSR `0x7849`。**

### 6. ESC 逻辑端口映射与物理标签相反

**这就是当初那句报错的根因**：

```text
error: No EtherCAT slave found on the network
```

当 EtherCAT0 / 物理 IN 口接上线时，PHY 地址 `2` 报告链路已连接。把该链路喂给 ESC 逻辑
端口 **0** 时，扫不到任何从站；喂给 ESC 逻辑端口 **1** 时，SOEM 立刻枚举出 `ECAT_Device`。

关键的实测值：

```text
错误的映射：
  0x782 = 47 50 00 00 00 11 03 00
  GPR_CFG2 = 0x11000000
  结果：找不到 EtherCAT 从站

正确的映射：
  0x782 = 47 50 00 00 10 10 03 00
  GPR_CFG2 = 0x10100000
  结果：发现 ECAT_Device，expected WKC 3
```

**除非有硬件证据支持更好的修法，否则请保持软件侧的链路映射交换。** 当前代码里由这个宏
表示：

```c
#define BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT (1)
```

其操作含义是：

```text
物理 EtherCAT0 / PHY 地址 2 -> ESC 逻辑端口 1
物理 EtherCAT1 / PHY 地址 1 -> ESC 逻辑端口 0
```

**这就是为什么两个 RJ45 看起来和物理 IN/OUT 标签是反的。** 在这个状态下单从站测试可以
正常工作。将来为了链式拓扑要改动它之前，请先分别测试两个物理接口，并明确记录期望的
逻辑拓扑。

曾经直接做过一次不交换的对照测试：

```c
#define BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT (0)
```

结果：主机返回 `No EtherCAT slave found on the network`。把
`BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT` 改回 `1` 之后，枚举与会话建立恢复正常。
因此**生产用的 hpm6e8y 桥固件应当保持 `swap = 1`**，同时在文档里仍把 EtherCAT0 记作
物理 IN 口。

### 7. 用软件 GPR 链路状态，不要用 LINK 输入引脚

片内 PHY 并不提供 EVK 上那些外部 PHY 所具备的稳定外部 LINK 引脚通路。SDK 默认那种
从 IO 取 `NMII_LINKx` 的做法**在这里不可用**。

固件通过 `GPR_CFG2` 驱动 ESC 的链路状态：

- `NMII_LINKx_FROM_IO = 0`
- `NMII_LINKx_GPR = 0` 表示链路有效
- `NMII_LINKx_GPR = 1` 表示链路无效

**不要把两个端口都强制置为已连接。** 只接一根线时，一个被谎报为"已连接"的空端口会让
ESC 无法闭合环路，主站可能因此收不到帧。

常规固件每 100 ms 轮询两个 PHY 的 BMSR 链路位并刷新 GPR 状态。**只在启动时轮询一次是
不够的**——那样在开机后才插线、或者经历一次瞬时断连之后，ESC 会保留过期的链路状态，
表现为"必须重新上电才能恢复"。

### 8. 启动时强制 100M MII 模式

干净的常规固件会显式配置：

- ESC 的 `PHY_CFG0.MAC_SPEED = 1`，即 100M
- ESC 的 `PORT0_RMII_EN/PORT1_RMII_EN/PORT2_RMII_EN = 0`
- JL1111 第 7 页寄存器 16 的 bit 3 清零，选择 MII

**在 EtherCAT 端口层释放 PHY 复位之后做一次即可。** 避免在桥运行期间反复写 PHY 的分页
寄存器。

### 9. 诊断用 CAN 与常规 CAN0

诊断镜像用 `PC00/PC01` 上的 MCAN0 来汇报 bring-up 帧，例如 `0x77f`、`0x780`..`0x789`、
`0x78a`、`0x78b`。

常规 EtherCAT 桥应用现在使用**同一组物理 CAN0 引脚**，只是没有那个诊断遥测任务。它的
逻辑主机 `CAN0` 是：

```text
逻辑 CAN0 = 物理丝印 CAN0 = HPM_MCAN0
TX/RX     = PC00/PC01
波特率     = 经典 CAN 1 Mbit/s
```

因此：

- 在 MCAN0/PC00-PC01 上看到诊断帧，可以证明物理 CAN0 通路是好的。
- 干净的常规桥**不会**发送 `0x77f/0x78x` 诊断帧。
- `ecat_board_test` 在逻辑 CAN0（即 MCAN0/PC00-PC01）上发 CAN ID `0x123`。
- **处于只听模式的 USB-CAN 适配器不会给板子的发送帧回 ACK。**
- 主机 example 只统计**收到的** CAN 帧，也就是说必须有东西向板子发帧才会计数；它不统计
  板子自己发出去的帧。

验证 CAN0 的步骤：

1. 接到物理 CAN0，它连的是 MCAN0/PC00-PC01。
2. 使用经典 CAN，1 Mbit/s。
3. 确保对端节点会回 ACK。**本测试不要用只听模式。**
4. 想让 `ecat_board_test` 打印 `[CAN0 RX]`，就在测试运行期间用外部节点向板子发任意一帧
   经典 CAN 帧。

### 10. UART0 回显需要物理环回

主机 example 在这里发送 UART0 文本：

```text
逻辑 UART0 = HPM_UART1
TX/RX      = PY07/PY06
波特率      = 921600
```

除非把 `PY07` 短接到 `PY06`，或者接一个会回数据的设备，否则
`done: 0 UART bytes received` **是预期结果**。

### 11. 诊断版 v4 有用，但它不是干净的常规固件

v4 诊断固件通过发送这些帧证明了端口映射：

```text
0x77f = ESTA 04
0x782 = GPR_CFG2 + MII_MNG_CS
0x783/0x784 = PHY BMCR/BMSR
0x785/0x786 = PHY ID
0x787/0x788/0x789 = ESC 配置/状态
0x78a/0x78b = JL1111 page7 RMSR
```

它同时也暴露出：**持续的诊断 CAN/PHY 访问不适合放进干净的常规桥**。干净的常规构建
移除了诊断 CAN 任务，也避免了周期性的 PHY 分页写。

## 症状对照表

| 症状 | 含义 | 处置 |
| --- | --- | --- |
| `No EtherCAT slave found` | 链路被上报到了错误的 ESC 逻辑端口 | 保持 PHY 链路到 ESC 端口的交换映射 |
| `ECAT_Device`、WKC 3、0 WKC 错误 | EtherCAT 传输健康 | 可以继续做 CAN/UART 的物理验证 |
| `SAFE-OP+ERROR 0x001B` | SyncManager 看门狗 | 先排除诊断固件的运行时干扰；若仍然存在再查 SSC/SM |
| 干净常规固件上收不到 `0x77f/0x78x` 帧 | 预期行为 | 常规桥固件里诊断 CAN 是关闭的 |
| `done: 0 CAN frames` | 没有从外部节点收到 CAN 帧 | 检查 MCAN0/PC00-PC01、1 Mbit 经典模式、ACK，以及外部是否真的在发 |
| `done: 0 UART bytes` | 没有 UART 环回或应答设备 | 用跳线把 PY07 短到 PY06，或接一个应答设备 |

## 已知未决事项

- **物理接口标签与 ESC 逻辑端口顺序相反**：物理 EtherCAT0/IN 目前是 ESC 逻辑端口 1，
  物理 EtherCAT1/OUT 是 ESC 逻辑端口 0。这一点已记录在案，且在单从站链路下工作正常，
  但**改代码之前应当先测试链式拓扑**。
- 常规桥目前只暴露一条逻辑 CAN 总线：物理 CAN0（MCAN0/PC00-PC01）。其余已确认可用的
  物理 CAN1..CAN3 端口尚未被 `RmcsBoardEcatBridge` 暴露出来。
