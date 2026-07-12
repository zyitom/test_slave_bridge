# hpm5321 - Hardware Pinout

SoC: **HPM5361** (`HPM5361xEGx`, QFN48; `openocd-soc: hpm5300`). The board is
*named* hpm5321 after the board series, but the silicon is HPM5361.

Every entry is taken directly from the firmware source; the "Source" column
lists where the pin is configured so the table stays verifiable.

## CAN (MCAN)

| Logical | Hardware | TXD  | RXD  | Mode    | Source                   |
| ------- | -------- | ---- | ---- | ------- | ------------------------ |
| CAN0    | MCAN0    | PA00 | PA01 | Classic | `app/board_app.cpp` `init_can()` |

Port table: `app/board_app.hpp` (`kCanPorts`). CAN0 is the DM (Damiao) motor
bus. The controller is configured classic-only (`CanMode::kClassic`): host
frames requesting CAN-FD are capped to classic on the wire (see README).

## UART

| Logical | Hardware | TXD  | RXD  | Baud   | Parity | Source                    |
| ------- | -------- | ---- | ---- | ------ | ------ | ------------------------- |
| UART0   | UART2    | PB08 | PB09 | 921600 | None   | `app/board_app.cpp` `init_uart()` |

Port table: `app/board_app.hpp` (`kUartPorts`). Both pins use internal
pull-up; RX additionally enables the Schmitt trigger. TX/RX are DMA-driven
(`HPM_DMA_SRC_UART2_TX/RX`), rings placed in `.ahb_sram` (naturally
non-cached on this SoC).

## LED (plain GPIO RGB, active-low)

Common-anode RGB LED: drive the pad LOW to light a channel
(`make_gpio_pin<..., false>`). Defined in `app/board_app.hpp`, muxed by
`init_led_pins()` in `app/board_app.cpp`.

| Color | Pin  |
| ----- | ---- |
| Blue  | PA29 |
| Green | PA30 |
| Red   | PA31 |

This board has no per-CAN indicator LEDs (`kCanIndicatorPins` is empty).

## USB (device, high speed)

`usb_use_high_speed()` returns true. Pins are set to analog and the DP/DM
45-ohm pull-downs are disconnected in `board.c`
(`board_init_usb_dp_dm_pins()`); VBUS is internal.

| Function | Pin  | Notes  |
| -------- | ---- | ------ |
| USB0 DM  | PA24 | Analog |
| USB0 DP  | PA25 | Analog |

## Buttons / Debug

| Function          | Pin  | Notes                                                        |
| ----------------- | ---- | ------------------------------------------------------------ |
| Bootloader button | PA07 | Shared with JTAG_TMS; sampled by `board_check_bootloader_force_stay_requested()` (`board.c`), then restored to JTAG_TMS so the debugger can attach |
| JTAG TCK          | PA04 | Default JTAG function; not muxed by the app                  |
| JTAG TDO          | PA05 | Default JTAG function; not muxed by the app                  |
| JTAG TDI          | PA06 | Default JTAG function; not muxed by the app                  |

## PY domain

| Pin  | Function      | Notes                                                   |
| ---- | ------------- | ------------------------------------------------------- |
| PY00 | SOC_GPIO_Y_00 | Switched to SoC GPIO domain in `board.c`, no peripheral |
| PY01 | SOC_GPIO_Y_01 | Switched to SoC GPIO domain in `board.c`, no peripheral |

## Clocks / timebases (firmware-relevant constants)

| Item                | Value                | Source                              |
| ------------------- | -------------------- | ----------------------------------- |
| MCHTMR0 (app timer) | 4 MHz (0.25 us tick) | `board.c`; `kMchtmrClockName`       |
| PTPC (CAN timestamp) | 160 MHz AHB, 6 ns step (`kCanTimestampNsPerUs = 960`) | `app/board_app.hpp` |
| Flash               | 1 MiB XPI NOR        | `board.h` (`BOARD_FLASH_SIZE`)      |
