# RMCS Slave HPM5321 DualCan - Pin Assignments

Board name: `RMCS_Slave_HPM5321_DualCan` (`BOARD_NAME`)
SoC: **HPM5361** (`HPM5361xEGx`, QFN48 package), named `hpm5321` after the board series
USB PID: `0xA902`

Two CAN-FD controllers and one data UART. No IMU, no DBUS receiver, no GPIO
application channels.

Build with `-DBOARD=hpm5321_dual_can`.

## Pin map — all confirmed

| Function        | Hardware  | Pad(s)              | Notes                    |
| --------------- | --------- | ------------------- | ------------------------ |
| CAN0            | MCAN0     | TX=PA00, RX=PA01    | CAN-FD, DM motor bus     |
| CAN1            | MCAN3     | TX=PA31, RX=PA30    | CAN-FD, second motor bus |
| UART            | UART2     | TX=PB08, RX=PB09    | 921600-8N1               |
| Main LED blue   | GPIO      | PA26                | Active-low               |
| Main LED green  | GPIO      | PA27                | Active-low               |
| Main LED red    | GPIO      | PA28                | Active-low               |
| CAN1 indicator  | HW LED    | PB14                | Driven by CAN transceiver|
| CAN2 indicator  | HW LED    | PB15                | Driven by CAN transceiver|
| JTAG            | default   | PA04/PA05/PA06/PA07 |                          |
| USB FS          | USB0      | DM=PA24, DP=PA25    | Analog, internal VBUS    |

## How the pin map was recovered

The schematic for this board was lost. All pins were recovered on the bench:

- **LED pins** (PA26/27/28, PB14/15): discovered by a GPIO blink-scan firmware
  (`boards/hpm5321_scan/`, `app/src/utility/pin_scan.hpp`)
- **CAN1** (MCAN0, PA00/PA01): carried over from the original `hpm5321` board
- **CAN2** (MCAN3, PA30/PA31): narrowed down by QFN48 pinmux exclusion after
  PB14/PB15 turned out to be CAN activity LEDs
- **UART** (UART2, PB08/PB09): same as original `hpm5321`; confirmed by exhaustive
  testing of all QFN48 UART TX/RXD pin pairs (variants 1–6) — only variant 1
  (PB08/PB09) produced data in rxmonitor
