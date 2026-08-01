# HPM6E8Y GPIO LED 引脚逆向

> **文档类型**：过程记录（硬件逆向）+ 硬件参考（LED 引脚表）
> **适用范围**：`firmware/rmcs_board/boards/hpm6e8y/`，无原理图的 BGA196 板
> **状态**：现行有效（15 个 LED 已确认；其中 PA25/PA28 事后重新归类为 PHY 保留引脚）
> **相关文档**：[README.md](README.md)（板级说明） · [CAN_PIN_REVERSE_ENGINEERING.md](CAN_PIN_REVERSE_ENGINEERING.md) · [ETHERNET_PIN_REVERSE_ENGINEERING.md](ETHERNET_PIN_REVERSE_ENGINEERING.md)

## 摘要

这块板（BGA196 的 hpm6e8y，带片内 PHY）有 **17 个高电平有效的 LED GPIO 输出**，但
**原理图/引脚表缺失**。本文件记录用扫描固件把它们找出来的全过程与最终结果。

**结论先行**（详见[扫描结果](#扫描结果2026-07-09-解码)）：找到 15 个 GPIO 可驱动的 LED，
全部落在 A/B/C/E bank，**没有一个在常开域 X/Y/Z**——早先"RGB 在 PZ 域"的猜测是错的，
RGB 实际在 bank E。另外**极性不统一**：主 RGB 和 EtherCAT0 黄灯是低电平有效，其余 11 个
指示灯是高电平有效。

**重要提醒**：`PA25` 和 `PA28` 事后经数据手册复核，被重新归类为**片内以太网 PHY 的
LED/地址 strap 引脚**，常规固件**不得**把它们当 GPIO 驱动。

扫描器固件的原理：逐个把候选 GPIO 拉高，同时通过已确认可用的 CAN0 口播报当前候选编号。

## 扫描策略：只排除已被证实占用的引脚

这块板上存在 1000M 以太网口和 EtherCAT 进/出口，但**它们的引脚尚未确认**，所以它们的
引脚被**刻意保留在扫描范围内**——那 17 个 LED 里任何一个都可能坐落在某个我们本会错误
归给网络走线的引脚上。候选表由 `app/led_pin_scanner.cpp` 生成，取全部六个 GPIO bank
（A-F，各 32 个引脚 = 192 个），再减去**只有那些在本板上功能确实已被证实**的引脚：

- UART0 控制台（板载 FT2232 调试桥）：PA00、PA01
- JTAG：PA04、PA05、PA06、PA07、PA08
- CAN0：PC00、PC01 —— CAN1：PB04、PB05 —— CAN2：PD08、PD09 —— CAN3：PD14、PD15
- USB0 ID/OC/PWR：PF19、PF22、PF23
- XPI0 启动 NOR flash（CS0/SCLK/DQS/D0-D3）：PB25-PB31。**core0 是从这块 flash 上 XIP
  执行的**，所以把其中任何一个重新复用成 GPIO 都会让 core0 在扫描途中硬件异常。PB24 只是
  没用到的第二片选（CA_CS1），可以扫描（在这块板上它是 KEYA 用户按键输入）。

**其余全部纳入扫描**——包括整个 bank E、ESC/RGMII 候选组，以及这个 BGA192 封装可能根本
没引出来的引脚。驱动一个未键合或专用的引脚是无害的：它只是不会点亮任何灯而已。
**这是安全的犯错方向**：过度排除可能永久性地漏掉一个真实的 LED，而多留几个无害候选
只是多花一点扫描时间。

### 常开域（bank X / Y / Z）

板上的 RGB 状态灯**不在** A-F bank 里：它位于常开域，紧挨着 MCAN4（PZ00/PZ01）和
UART1（PY06/PY07）。这正是为什么只扫 A-F 会留下三个灯在上电时常亮、且永远关不掉。
因此扫描器也覆盖常开域引脚——这些引脚需要额外写一次路由寄存器才能接到 SoC GPIO
（bank X 是普通 IOC，bank Y 经 PIOC，bank Z 经 BIOC，都走 ALT 功能 3）：

- Bank X：PX00-PX07（全部八个）
- Bank Y：**只扫** PY02、PY03、PY04 —— PY00/PY01（电源域 UART）和 PY05
  （`PWDG_RSTN` 看门狗复位）盲翻电平不安全；PY06/PY07 是 UART1
- Bank Z：PZ03-PZ07 —— PZ00-PZ02 是 MCAN4 的 TXD/RXD/STBY。PZ03-PZ07 是当时认为
  最可能的 RGB 通道。

最终候选集有 **183 个**（A-F 里 167 个，加常开域 16 个）。

> ⚠️ **不要相信** `board.h` / `app/board_app.hpp` 里早先那些 RGB / EtherCAT LED 引脚
> 猜测值——它们未经验证，而本次扫描的目的正是取代它们。

## 构建与烧录

```bash
cmake -S firmware/rmcs_board/ecat -B firmware/rmcs_board/ecat/build_led_scan -G Ninja \
    -DBOARD=hpm6e8y \
    -DRMCS_ECAT_CORE1_LED_PIN_SCANNER=ON \
    -DRMCS_ECAT_CORE1_CAN_PIN_SCANNER=OFF \
    -DRMCS_ECAT_CORE1_LOOPBACK=OFF
cmake --build firmware/rmcs_board/ecat/build_led_scan
```

通过已有的 bootloader 烧录这个 DFU 产物：

```bash
dfu-util -d a11c:a904 -a 0 -D firmware/rmcs_board/ecat/build_led_scan/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

## 怎么记录 LED 引脚

每个候选引脚会被驱动两个时间窗，这样两种 LED 都能抓到：约 **700 ms 的高电平窗**
（阶段 `H`）用来发现平时熄灭、被拉高后点亮的灯；约 **500 ms 的低电平窗**（阶段 `L`）
用来发现复位后就处于高电平的灯（板上有几个这样的——多半是 RGB 通道，因为固件没有把
LED 引脚归位，所以它们会一直亮到扫描把该引脚拉低为止）。
**两种现象都要盯**：`H` 窗里有灯亮起，以及 `L` 窗里有常亮的灯熄灭。

1. 把 USB2CAN 接到已确认的 CAN0 接口，1 Mbps 经典 CAN。
2. **在给板子上电或复位之前**就开始抓 CAN 日志。
3. 扫描器在每个候选期间持续发送标准 ID `0x700 + 编号`。
4. 当某个物理 LED **亮起**时，记下最近一帧 `H`；当某个常亮 LED **熄灭**时，记下最近一帧
   `L`。两种情况下 `byte2`/`byte3` 都是引脚。
5. 负载解码：

```text
byte0 = 0x4c（'L'）
byte1 = 候选编号
byte2 = GPIO bank 的 ASCII，例如 0x45 = 'E'
byte3 = GPIO 引脚号
byte4 = 阶段：'B' 开始，'H' 已拉高，'L' 已拉低
byte5 = 候选总数，当前为 183
```

CAN 标准 ID 等于 `0x700 + 候选编号`，而 `byte2`/`byte3` 携带当前被驱动引脚的 bank ASCII
和引脚号，所以从 ID 或负载都能识别出是哪个引脚。

**观察到 17 个不同的 LED 之后即可停止**。那 17 组 bank/引脚就是板上的 LED GPIO 引脚表。

## 扫描结果（2026-07-09 解码）

最初观察到的 15 个 GPIO 可驱动 LED，已于 2026-07-10 由操作者对照确认镜像的闪烁顺序
逐一核实。之后对照 HPM6E\*Y\* 数据手册的复核**重新归类了 `PA25` 和 `PA28`**：它们是
片内以太网 PHY 的 LED / PHY 地址 strap 引脚，**不是可以安全使用的应用 GPIO LED**。
常规固件现在不再驱动它们，把它们留给 PHY。

余下的安全 GPIO LED 分布在 bank A/B/C/E ——**常开域 X/Y/Z 里一个都没有**，所以早先
"RGB 在 PZ 域"的猜测是错的；RGB 在 bank E。`board.c` 的 `board_park_leds_off()` 现在
只在启动时把这些安全 GPIO LED 驱动到熄灭状态。

**极性并不统一**：主 RGB 是**低电平有效**（共阳极），EtherCAT0 黄灯（PA25）也是；
其余 11 个指示灯是**高电平有效**。逐灯极性见下表。
注意：扫描的 `H`/`L` 阶段本身**并不能证明极性**（一个常亮的低有效 LED 在 `H` 窗里同样
会变化）；RGB 的极性是单独钉死的——原始固件之所以让绿灯（PE04）一直亮着，恰恰是因为它
把那个引脚拉低了，而只有低有效的 LED 才会因此点亮。

| LED（物理标识） | 引脚 | ID | 极性 | 备注 |
|------------------------|------|--------|------------|-----------------|
| 主 RGB - 红 | PE05 | 0x76f | 低有效 | 上电时点亮 |
| 主 RGB - 绿 | PE04 | 0x76e | 低有效 | 上电时点亮 |
| 主 RGB - 蓝 | PE03 | 0x76d | 低有效 | 上电时点亮 |
| CAN0 - 绿 | PC26 | 0x748 | 高有效 | |
| CAN0 - 蓝 | PC27 | 0x749 | 高有效 | |
| CAN1 - 绿 | PE00 | 0x76a | 高有效 | |
| CAN1 - 蓝 | PE02 | 0x76c | 高有效 | |
| CAN2 - 绿 | PA09 | 0x702 | 高有效 | |
| CAN2 - 蓝 | PB00 | 0x719 | 高有效 | |
| CAN3 - 绿 | PB02 | 0x71b | 高有效 | |
| CAN3 - 蓝 | PB03 | 0x71c | 高有效 | |
| PHY1 LED/strap | PA25 | 0x712 | **保留** | PHY LED1 / addr1 |
| PHY0 LED/strap | PA28 | 0x715 | **保留** | PHY LED1 / addr1 |
| EtherCAT 中间 - 绿 | PC20 | 0x742 | 高有效 | |
| EtherCAT 中间 - 红 | PC21 | 0x743 | 高有效 | |

这是最初那轮 GPIO LED 扫描观察到的完整集合，但经片内 PHY 复核后，`PA25` 和 `PA28` 属于
**保留**。强行把它们当 GPIO 驱动时可能仍会看到黄灯有反应，但**常规固件绝不能这么做**，
因为同样这两个引脚会锁存 PHY 地址，并且设计用途就是 PHY 的 LED 输出。

最初上电时有三个灯亮着：主蓝 PE03、主绿 PE04（都是低有效，其引脚在复位后处于低电平），
以及 PA25 这个 PHY LED/strap 引脚。RGB 可以安全地归位到高电平（熄灭）；而 **PA25 必须
留给 PHY**，不要用强制 GPIO 的方式绕过。

### 这些结果推翻了几处此前假定的引脚分配（现已全部解决）

- `board.h` / `board_app.hpp` 里猜的 RGB 是 PE14/PE15/PE04。**实际 RGB 是
  PE05(R)/PE04(G)/PE03(B)，且为低电平有效**。已修复：`board_app.hpp` 的引脚与极性，
  以及 `board.c` 的 `board_turnoff_rgb_led()` 现在把它们驱动为高（熄灭）。
- `board_app.cpp` 的 `init_can_indicator_pins()` 写着"没有每路 CAN 的指示灯"。**这是
  错的**：每个 CAN 口都有绿+蓝两个指示灯（高有效）。注释已更正；但这些引脚目前尚未
  真正接成指示灯功能。
- 原来那份 `board.c` 的 `init_esc_pins()`（从 EVK 抄来的）把 ESC 功能配到了真实的 LED
  引脚上：PA09=REFCK 其实是 CAN2-绿，PA25=TXD_0 是 EtherCAT0-黄，PA28=TXD_3 是
  EtherCAT1-黄，PE02=CTR_6 是 CAN1-蓝，PE03=CTR_1 是主-蓝。**一个引脚不可能既是 MII
  线又是静态 GPIO LED**，所以这套引脚复用在本板上根本行不通，而且它会在 LED 引脚被归位
  之后又把它们抢回去（表现为主蓝灯一直亮着）。
  **已解决**：常规固件现在使用 HPM6E\*Y\* 的**片内 PHY 映射**。`PA16..PA29` 是模拟/strap
  引脚，`PV00..PV15` 和 `PW00..PW15` 是片内 MII 总线，`PV12`/`PW12` 是低有效的 PHY 复位，
  而 `PA25`/`PA28` 保留为 PHY 的 LED/地址 strap 引脚，既不当 GPIO LED，也不当外部 MII
  数据线。

### 两个非 GPIO 的 LED

板上还有 ENET 1000M 和 100M 的链路状态灯。它们由**以太网 PHY 自己的 LED 输出引脚**驱动
（硬件级的链路/速率指示），不经过 MCU GPIO，所以扫描器点不亮它们。它们不属于那 15 个
GPIO LED。

## LED 确认镜像

在上面这份 15 灯映射已知之后，用这个镜像来验证它：启动时先把所有已知 LED 驱动到熄灭
（按各自极性处理，顺便也把否则会浮高的 EtherCAT0 引脚归位），然后依次让每个 LED 闪两下。
每个 LED 之前会先发一帧 CAN0 播报帧，这样就能把正在闪的物理 LED 和它的引脚对上号。

```bash
cmake -S firmware/rmcs_board/ecat -B firmware/rmcs_board/ecat/build_led_confirm -G Ninja \
    -DBOARD=hpm6e8y -DRMCS_ECAT_CORE1_LED_CONFIRM=ON
cmake --build firmware/rmcs_board/ecat/build_led_confirm
dfu-util -d a11c:a904 -a 0 -D firmware/rmcs_board/ecat/build_led_confirm/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

播报帧（标准 ID `0x700 + LED 序号`，顺序即上面的表）：

```text
byte0 = 0x43（'C'）
byte1 = LED 序号（0..14）
byte2 = GPIO bank 的 ASCII
byte3 = GPIO 引脚号
byte4 = 极性：'H' 高有效，'L' 低有效
byte5 = LED 总数（15）
```

## USB 运行时 DFU 说明

EtherCAT core0 应用现在会枚举一个 TinyUSB 的 DFU-runtime 接口，VID:PID 为 `a11c:a904`。
烧完 app 之后，`lsusb` 里应该仍能看到板子处于 runtime 模式。从 runtime 发起的 DFU
detach 请求会写入与常规 app 相同的 boot mailbox，然后复位进入 bootloader 的 DFU 模式。

### 已知问题（复核后降级为"未复现"）：DFU-runtime 的 detach 在板子跑一段时间后会失效

> **2026-08-01 复核**：本节末尾提出的"把 ESC bring-up 关掉之后应该就可靠了，但仍需
> 验证与加固"——**验证做了，本次未复现**。当天在核对调布局与单核镜像之间来回刷了
> 十余次，每次都是 app runtime 状态下 `dfu-util` 自动 detach + 重枚举成功，没有出现
> 一次 `Failed to retrieve language identifiers`；期间还夹着 900 秒 16 kHz 满载压测和
> 多次 EtherCAT/USB 会话。
>
> 但**不宣布已修复**：原因分析（core0 停止调用 `tud_task()`）指向的是"主循环被卡住"
> 这一类故障，而本次的所有镜像里主循环都是健康的，等于没有制造出触发条件。
> 结论应表述为"在当前固件下未观察到"，而不是"已解决"。下面的原文与恢复办法保留。

板子运行一段时间后，`dfu-util -d a11c:a904` 有时会失败，报 `Failed to retrieve language
identifiers` / `Cannot set alternate interface zero: LIBUSB_ERROR_OTHER`。设备仍然能被
枚举（`lsusb` 里看得到），但**不再响应 USB 控制传输**，原因是 core0 停止调用
`tud_task()`（`usb_runtime.cpp`）——在旧的 ESC 桥路径里，`ecat_hardware_init` 失败时
core0 会从 `main()` 返回，或者 SSC 的 `MainLoop` 卡住了。把 ESC bring-up 关掉之后
（core0 现在永远循环调用 `tud_task()`）应该就可靠了，但这一点**仍需验证与加固**。

**发生时的恢复办法**：按住 KEYA 按键（PB24）的同时复位或重新上电——此时 bootloader
（`bootloader/src/main.cpp` 的 `board_check_bootloader_force_stay_requested`）会停在它
自己的 DFU 模式里，**与卡死的 app 无关**，然后
`dfu-util -d a11c:a904 -a 0 -D <镜像>` 就能直接写入。
