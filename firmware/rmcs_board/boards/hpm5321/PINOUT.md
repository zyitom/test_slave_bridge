# hpm5321 —— 硬件引脚分配

> **文档类型**：硬件参考（引脚表）
> **适用范围**：`firmware/rmcs_board/boards/hpm5321/`
> **状态**：现行有效
> **相关文档**：[README.md](README.md)（板级说明） · [../hpm5321_dual_can/PINOUT.md](../hpm5321_dual_can/PINOUT.md)（双 CAN 版对照）

## 摘要

本文件是 hpm5321 的**引脚速查表**。每一项都直接取自固件源码，"来源"列标明该引脚在哪里
被配置，**便于随时回源核对**——发现表与代码不一致时，以代码为准并回来更正本表。

SoC 是 **HPM5361**（`HPM5361xEGx`，QFN48；`openocd-soc: hpm5300`）。注意板子**名字**叫
hpm5321 是沿用板系列名，实际硅片是 HPM5361。

## CAN（MCAN）

| 逻辑名 | 硬件 | TXD | RXD | 模式 | 来源 |
| ------- | -------- | ---- | ---- | ------- | ------------------------ |
| CAN0 | MCAN0 | PA00 | PA01 | 经典 | `app/board_app.cpp` 的 `init_can()` |

端口表在 `app/board_app.hpp`（`kCanPorts`）。CAN0 是 DM（达妙）电机总线。控制器配置为
仅经典模式（`CanMode::kClassic`）：主机请求 CAN-FD 的帧上线时会被降为经典，见
[README.md](README.md)。

## UART

| 逻辑名 | 硬件 | TXD | RXD | 波特率 | 校验 | 来源 |
| ------- | -------- | ---- | ---- | ------ | ------ | ------------------------- |
| UART0 | UART2 | PB08 | PB09 | 921600 | 无 | `app/board_app.cpp` 的 `init_uart()` |

端口表在 `app/board_app.hpp`（`kUartPorts`）。两个引脚都启用了内部上拉；RX 额外开了
施密特触发器。收发均由 DMA 驱动（`HPM_DMA_SRC_UART2_TX/RX`），环形缓冲放在
`.ahb_sram`（在这颗 SoC 上天然是非缓存的）。

## LED（普通 GPIO 驱动的 RGB，低电平有效）

共阳极 RGB 灯：把引脚**拉低**才点亮对应通道（`make_gpio_pin<..., false>`）。定义在
`app/board_app.hpp`，由 `app/board_app.cpp` 的 `init_led_pins()` 完成引脚复用。

| 颜色 | 引脚 |
| ----- | ---- |
| 蓝 | PA29 |
| 绿 | PA30 |
| 红 | PA31 |

这块板**没有**每路 CAN 的独立指示灯（`kCanIndicatorPins` 为空）。

## USB（设备模式，高速）

`usb_use_high_speed()` 返回 true。引脚设为模拟功能，并且在 `board.c` 的
`board_init_usb_dp_dm_pins()` 里断开了 DP/DM 的 45 欧下拉；VBUS 走内部。

| 功能 | 引脚 | 说明 |
| -------- | ---- | ------ |
| USB0 DM | PA24 | 模拟 |
| USB0 DP | PA25 | 模拟 |

## Bootloader 强制常驻引脚 / 调试

> 这里的「触发」不一定是按键：`board.c` 采的是**与 JTAG_TMS 复用的引脚**，函数名里的
> `button` 只是沿用叫法。同系列的 `hpm5321_dual_can` 已确认板上没有任何实体键
> [实测 2026-08-03]，本板是否装了键以实物为准 [推断]。

| 功能 | 引脚 | 说明 |
| ----------------- | ---- | ------------------------------------------------------------ |
| Bootloader 强制常驻 | PA07 | 与 JTAG_TMS 复用。复位后由 `board.c` 的 `board_check_bootloader_force_stay_requested()` 采样：内部上拉，**需拉到 GND 才算触发**；窗口只有 4×250us≈1ms，采样完毕后**恢复为 JTAG_TMS**，以便调试器仍能连接 |
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
