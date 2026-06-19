# RMCS Slave HPM5321 - Pin Assignments

Board name: `RMCS_Slave_HPM5321` (`BOARD_NAME`)
SoC: **HPM5361** (`device: HPM5361xEGx`, `openocd-soc: hpm5300`) - the board is
*named* hpm5321, but the silicon is HPM5361.
USB PID: `0xA901`

This board is a minimal bridge: one CAN controller, one data UART, and a plain
GPIO RGB LED. It has no IMU, no DBUS receiver, no GPIO application channels and
no USB speed switch.

Every entry below is taken directly from the firmware source. The "Source"
column lists where the pin is configured so the table stays verifiable.

## CAN (MCAN)

| Logical | Hardware | TXD  | RXD  | Mode    | Source                 |
| ------- | -------- | ---- | ---- | ------- | ---------------------- |
| CAN0    | MCAN0    | PA00 | PA01 | Classic | board_app.cpp init_can |

`CanPort` table: `app/board_app.hpp` (`kCanPorts`). Pin mux: `init_can()` in
`app/board_app.cpp`. CAN0 is the DM (Damiao) motor bus.

## UART

| Logical | Hardware | TXD  | RXD  | Baud   | Parity | Source                  |
| ------- | -------- | ---- | ---- | ------ | ------ | ----------------------- |
| UART0   | UART2    | PB08 | PB09 | 921600 | None   | board_app.cpp init_uart |

`UartPort` table: `app/board_app.hpp` (`kUartPorts`). Pin mux: `init_uart()` in
`app/board_app.cpp`. Both pins use internal pull-up; RX adds a Schmitt trigger.

## LED (plain GPIO RGB, active-low)

Common-anode RGB LED: drive the pad LOW to light a channel
(`make_gpio_pin<..., false>` = active-low). Defined in `app/board_app.hpp`,
configured by `init_led_pins()` in `app/board_app.cpp`.

| Color | Pin  |
| ----- | ---- |
| Blue  | PA29 |
| Green | PA30 |
| Red   | PA31 |

## Buttons / Debug

| Function           | Pin  | Notes                                                                |
| ------------------ | ---- | -------------------------------------------------------------------- |
| Bootloader button  | PA07 | Shared with JTAG_TMS; sampled by `board_check_bootloader_force_stay_requested()`, then restored to JTAG_TMS |
| JTAG TCK           | PA04 | Default JTAG function; not muxed by the app                          |
| JTAG TDO           | PA05 | Default JTAG function; not muxed by the app                          |
| JTAG TDI           | PA06 | Default JTAG function; not muxed by the app                          |

## USB (internal VBUS, high speed)

`usb_use_high_speed()` returns true; pins set to analog in `board.c`
(`board_init_usb_dp_dm_pins`).

| Function | Pin  | Notes  |
| -------- | ---- | ------ |
| USB0 DM  | PA24 | Analog |
| USB0 DP  | PA25 | Analog |

## PY domain

| Pin  | Function     | Notes                                              |
| ---- | ------------ | -------------------------------------------------- |
| PY00 | SOC_GPIO_Y_00 | Switched to SoC GPIO domain in `board.c`, no peripheral |
| PY01 | SOC_GPIO_Y_01 | Switched to SoC GPIO domain in `board.c`, no peripheral |
