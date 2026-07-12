# RMCS Slave HPM5321

Minimal USB<->fieldbus forwarding bridge: one classic CAN controller, one data
UART, and a plain GPIO RGB status LED. No IMU, no DBUS receiver, no GPIO
application channels, no per-CAN indicator LEDs.

Hardware pin assignments live in [PINOUT.md](PINOUT.md).

## Identifiers

| Item        | Value                             |
| ----------- | --------------------------------- |
| Board name  | `RMCS_Slave_HPM5321` (`BOARD_NAME`) |
| SoC         | HPM5361 (QFN48), single RV32 core |
| USB PID     | `0xA901` (VID `0xA11C`)           |
| USB speed   | High speed (480 Mbps)             |
| Flash       | 1 MiB XPI NOR, app behind the shared DFU bootloader |

## What the host sees

The board runs the shared rmcs_board USB application (`app/`): a USB
vendor-class device carrying the librmcs byte stream, with the session
handshake (kStart nonce + keepalive lease) handled by the shared
`link::HostSession`.

| Host endpoint | Board resource | Notes                          |
| ------------- | -------------- | ------------------------------ |
| `DataId::kCan0` | MCAN0 (classic 1 Mbps) | DM (Damiao) motor bus |
| `DataId::kUart0` | UART2, 921600-8N1     | DMA-driven both directions |

### Classic-only CAN and the per-frame FD flag

The shared CAN driver honors the host's per-frame `is_fdcan` flag capped by
controller capability (`send_fd = canfd_ && data.is_fdcan`). This board
configures MCAN0 classic-only, so `is_fdcan = true` frames are transmitted as
classic CAN 2.0 -- safe, but silently capped. Use `hpm5321_dual_can` if you
need CAN-FD on the wire.

## Build and flash

```bash
cmake --preset debug -S firmware/rmcs_board -DBOARD=hpm5321
cmake --build firmware/rmcs_board/build
```

The build produces `rmcs_board_app_hpm5321.dfu` under
`firmware/rmcs_board/build/app/output/`. Flash over USB DFU (the shared RMCS
DFU bootloader must already be on the chip; first-time bootloader flashing
needs a debugger):

```bash
dfu-util -d 0xa11c:0xa901 -a 0 -D firmware/rmcs_board/build/app/output/rmcs_board_app_hpm5321.dfu
```

A running app exposes the DFU runtime interface, so dfu-util detaches and
re-enumerates it into DFU mode automatically. To force the bootloader to stay
resident, hold the PA07 button through reset (or power up with no valid app).

## Status LED light language

Single RGB LED (see PINOUT.md for pins), driven by the shared Led driver
(`app/src/led/led.hpp`). Yellow is red+green, cyan is green+blue; priority is
top to bottom:

| Pattern               | Meaning                                                   |
| --------------------- | --------------------------------------------------------- |
| yellow/cyan alternate | both directions congested (uplink AND downlink full)      |
| yellow blink (~4 Hz)  | uplink buffer full (board -> host); host not draining     |
| cyan blink (~4 Hz)    | downlink buffer full (host -> board); CAN TX backed up    |
| steady green          | host session established, data forwarding                 |
| slow green blink 1 Hz | alive, waiting for a host session                         |

"Host session established" means the librmcs handshake is up (kStart nonce
acknowledged and the keepalive lease refreshed within 1 s) -- NOT merely that
the USB cable is plugged in. An enumerated device without a live host
application stays on the slow green blink.
