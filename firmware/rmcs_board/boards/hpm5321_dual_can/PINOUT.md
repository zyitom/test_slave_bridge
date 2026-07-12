# hpm5321_dual_can - Hardware Pinout

SoC: **HPM5361** (`HPM5361xEGx`, QFN48), named `hpm5321` after the board
series. The schematic for this board was lost; every pin below was recovered
on the bench and is confirmed working (see "How the pin map was recovered").

Every entry is taken directly from the firmware source; the "Source" column
lists where the pin is configured so the table stays verifiable.

## CAN (MCAN)

| Logical | Hardware | TXD  | RXD  | Mode   | Source                   |
| ------- | -------- | ---- | ---- | ------ | ------------------------ |
| CAN0    | MCAN0    | PA00 | PA01 | CAN-FD | `app/board_app.cpp` `init_can()` |
| CAN1    | MCAN3    | PA31 | PA30 | CAN-FD | `app/board_app.cpp` `init_can()` |

Port table: `app/board_app.hpp` (`kCanPorts`). Both controllers run CAN-FD
(arbitration 1 Mbps, data 5 Mbps); the frame type is chosen per frame by the
host (see README). CAN0 is the DM (Damiao) motor bus.

Silkscreen note: the board labels the buses CAN1/CAN2, firmware numbers them
CAN0/CAN1 (0-based). See the indicator LED table below for the full mapping.

## UART

| Logical | Hardware | TXD  | RXD  | Baud   | Parity | Source                    |
| ------- | -------- | ---- | ---- | ------ | ------ | ------------------------- |
| UART0   | UART2    | PB08 | PB09 | 921600 | None   | `app/board_app.cpp` `init_uart()` |

Port table: `app/board_app.hpp` (`kUartPorts`). Both pins use internal
pull-up; RX additionally enables the Schmitt trigger. TX/RX are DMA-driven
(`HPM_DMA_SRC_UART2_TX/RX`), rings placed in `.ahb_sram` (naturally
non-cached on this SoC).

## LEDs

Main RGB LED is common-anode, active-low (`make_gpio_pin<..., false>`); the
per-CAN indicator LEDs are active-high (anode to pin). Defined in
`app/board_app.hpp`, muxed by `init_led_pins()` / `init_can_indicator_pins()`.

| Function       | Pin  | Polarity    | Notes                                |
| -------------- | ---- | ----------- | ------------------------------------ |
| Main LED blue  | PA26 | Active-low  |                                      |
| Main LED green | PA27 | Active-low  |                                      |
| Main LED red   | PA28 | Active-low  |                                      |
| CAN0 indicator | PB14 | Active-high | Silkscreen "CAN1" (MCAN0, PA00/PA01) |
| CAN1 indicator | PB15 | Active-high | Silkscreen "CAN2" (MCAN3, PA31/PA30) |

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
| Bootloader button | PA07 | Shared with JTAG_TMS; sampled by `board_check_bootloader_force_stay_requested()` (`board.c`), then restored to JTAG_TMS |
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

## How the pin map was recovered

The schematic for this board was lost. All pins were recovered on the bench:

- **LED pins** (PA26/27/28, PB14/15): discovered by a GPIO blink-scan firmware
  (a temporary `boards/hpm5321_scan/` board with `pin_scan.hpp`; since removed,
  available in git history)
- **CAN0** (MCAN0, PA00/PA01): carried over from the original `hpm5321` board
- **CAN1** (MCAN3, PA31/PA30): narrowed down by QFN48 pinmux exclusion after
  PB14/PB15 turned out to be CAN activity LEDs
- **UART** (UART2, PB08/PB09): same as original `hpm5321`; confirmed by
  exhaustive testing of all QFN48 UART TXD/RXD pin pairs (variants 1-6) --
  only variant 1 (PB08/PB09) produced data in rxmonitor
