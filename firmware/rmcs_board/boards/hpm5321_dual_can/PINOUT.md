# hpm5321_dual_can —— 硬件引脚分配

> **文档类型**：硬件参考（引脚表）+ 过程记录（引脚恢复方法）
> **适用范围**：`firmware/rmcs_board/boards/hpm5321_dual_can/`
> **状态**：现行有效（全部引脚已在实机上验证可用）
> **相关文档**：[README.md](README.md)（板级说明与 CAN 故障排查） · [../hpm5321/PINOUT.md](../hpm5321/PINOUT.md)（单 CAN 版对照）

## 摘要

本文件是 hpm5321_dual_can 的**引脚速查表**。每一项都直接取自固件源码，"来源"列标明该
引脚在哪里被配置，**便于随时回源核对**。

**这块板的原理图已经丢失**，下面每一个引脚都是在实机上逐个试出来并确认可用的——恢复
方法记录在文末[引脚表是怎么恢复出来的](#引脚表是怎么恢复出来的)一节，将来再遇到无图
的板子可以照这个路子走。

SoC 是 **HPM5361**（`HPM5361xEGx`，QFN48），名字里的 `hpm5321` 是沿用板系列名。

## CAN（MCAN）

| 逻辑名 | 硬件 | TXD | RXD | 模式 | 来源 |
| ------- | -------- | ---- | ---- | ------ | ------------------------ |
| CAN0 | MCAN0 | PA00 | PA01 | CAN-FD | `app/board_app.cpp` 的 `init_can()` |
| CAN1 | MCAN3 | PA31 | PA30 | CAN-FD | `app/board_app.cpp` 的 `init_can()` |

端口表在 `app/board_app.hpp`（`kCanPorts`）。两个控制器都跑 CAN-FD（仲裁 1 Mbps，
数据 5 Mbps）；帧类型由主机逐帧选择，见 [README.md](README.md)。CAN0 是 DM（达妙）
电机总线。

**丝印提示**：板上把两条总线标为 CAN1/CAN2，而固件按 0 起编号叫 CAN0/CAN1。完整对应
关系见下面的指示灯表。

## UART

| 逻辑名 | 硬件 | TXD | RXD | 波特率 | 校验 | 来源 |
| ------- | -------- | ---- | ---- | ------ | ------ | ------------------------- |
| UART0 | UART2 | PB08 | PB09 | 921600 | 无 | `app/board_app.cpp` 的 `init_uart()` |

端口表在 `app/board_app.hpp`（`kUartPorts`）。两个引脚都启用了内部上拉；RX 额外开了
施密特触发器。收发均由 DMA 驱动（`HPM_DMA_SRC_UART2_TX/RX`），环形缓冲放在
`.ahb_sram`（在这颗 SoC 上天然是非缓存的）。

## LED

主 RGB 灯是**共阳极、低电平有效**（`make_gpio_pin<..., false>`）；每路 CAN 的指示灯是
**高电平有效**（阳极接引脚）。定义在 `app/board_app.hpp`，由 `init_led_pins()` /
`init_can_indicator_pins()` 完成引脚复用。

| 功能 | 引脚 | 极性 | 说明 |
| -------------- | ---- | ----------- | ------------------------------------ |
| 主灯 蓝 | PA26 | 低有效 | |
| 主灯 绿 | PA27 | 低有效 | |
| 主灯 红 | PA28 | 低有效 | |
| CAN0 指示灯 | PB14 | 高有效 | 丝印为 "CAN1"（MCAN0，PA00/PA01） |
| CAN1 指示灯 | PB15 | 高有效 | 丝印为 "CAN2"（MCAN3，PA31/PA30） |

## USB（设备模式，高速）

`usb_use_high_speed()` 返回 true。引脚设为模拟功能，并且在 `board.c` 的
`board_init_usb_dp_dm_pins()` 里断开了 DP/DM 的 45 欧下拉；VBUS 走内部。

| 功能 | 引脚 | 说明 |
| -------- | ---- | ------ |
| USB0 DM | PA24 | 模拟 |
| USB0 DP | PA25 | 模拟 |

## Bootloader 强制常驻引脚 / 调试

> **本板没有任何实体按键** [实测 2026-08-03]。`board.c` 里那个函数名叫
> `..._force_stay_button_pin()`，但它采的是一个**与 JTAG_TMS 复用的引脚**，不是键。
> 有实体键的是 `hpm6e8y`（`/* User key KEYA = PB24 */`），别把两块板搞混。

| 功能 | 引脚 | 说明 |
| ----------------- | ---- | ------------------------------------------------------------ |
| Bootloader 强制常驻 | PA07 | **与 JTAG_TMS 复用，板上无按键**。复位后由 `board.c` 的 `board_check_bootloader_force_stay_requested()` 采样：内部上拉，**需外部拉到 GND 才算触发**；窗口只有 4×250us≈1ms，之后恢复为 JTAG_TMS |
| JTAG TCK | PA04 | 默认 JTAG 功能，app 不做复用 |
| JTAG TDO | PA05 | 默认 JTAG 功能，app 不做复用 |
| JTAG TDI | PA06 | 默认 JTAG 功能，app 不做复用 |

## PY 域

| 引脚 | 功能 | 说明 |
| ---- | ------------- | ------------------------------------------------------- |
| PY00 | SOC_GPIO_Y_00 | 在 `board.c` 里切到 SoC GPIO 域，不接外设 |
| PY01 | SOC_GPIO_Y_01 | 在 `board.c` 里切到 SoC GPIO 域，不接外设 |

## 时钟 / 时基（与固件相关的常量）

| 项目 | 取值 | 来源 |
| ------------------- | -------------------- | ----------------------------------- |
| MCHTMR0（app 定时器） | 4 MHz（0.25 us 一个 tick） | `board.c`；`kMchtmrClockName` |
| PTPC（CAN 时间戳） | 160 MHz AHB，6 ns 步进（`kCanTimestampNsPerUs = 960`） | `app/board_app.hpp` |
| Flash | 1 MiB XPI NOR | `board.h`（`BOARD_FLASH_SIZE`） |

## 引脚表是怎么恢复出来的

这块板的原理图丢了，所有引脚都是在实机上逐个恢复的：

- **LED 引脚**（PA26/27/28、PB14/15）：用一份 GPIO 闪灯扫描固件找出来的（当时临时建了
  一个 `boards/hpm5321_scan/` 板级目录，配 `pin_scan.hpp`；现已删除，可在 git 历史里找到）。
- **CAN0**（MCAN0，PA00/PA01）：沿用原始 `hpm5321` 板的配置。
- **CAN1**（MCAN3，PA31/PA30）：在确认 PB14/PB15 其实是 CAN 活动指示灯之后，按 QFN48
  的引脚复用可能性做排除法缩小范围得到。
- **UART**（UART2，PB08/PB09）：与原始 `hpm5321` 相同；通过穷举 QFN48 上所有 UART
  TXD/RXD 引脚组合（变体 1-6）确认——只有变体 1（PB08/PB09）能在 rxmonitor 里看到数据。
