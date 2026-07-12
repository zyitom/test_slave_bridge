# RMCS Slave HPM5321 DualCan

USB<->fieldbus forwarding bridge with two CAN-FD controllers, one data UART, a
main RGB status LED and one indicator LED per CAN bus. No IMU, no DBUS
receiver, no GPIO application channels.

Hardware pin assignments live in [PINOUT.md](PINOUT.md), including how the pin
map was recovered after the schematic was lost.

## Identifiers

| Item        | Value                                     |
| ----------- | ----------------------------------------- |
| Board name  | `RMCS_Slave_HPM5321_DualCan` (`BOARD_NAME`) |
| SoC         | HPM5361 (QFN48), single RV32 core         |
| USB PID     | `0xA902` (VID `0xA11C`)                   |
| USB speed   | High speed (480 Mbps)                     |
| Flash       | 1 MiB XPI NOR, app behind the shared DFU bootloader |

## What the host sees

The board runs the shared rmcs_board USB application (`app/`): a USB
vendor-class device carrying the librmcs byte stream, with the session
handshake (kStart nonce + keepalive lease) handled by the shared
`link::HostSession`.

| Host endpoint    | Board resource                  | Notes                     |
| ---------------- | ------------------------------- | ------------------------- |
| `DataId::kCan0`  | MCAN0, CAN-FD 1 Mbps / 5 Mbps   | DM (Damiao) motor bus     |
| `DataId::kCan1`  | MCAN3, CAN-FD 1 Mbps / 5 Mbps   | Second motor bus          |
| `DataId::kUart0` | UART2, 921600-8N1               | DMA-driven both directions |

## Build and flash

```bash
cmake --preset debug -S firmware/rmcs_board -DBOARD=hpm5321_dual_can
cmake --build firmware/rmcs_board/build
```

Flash over USB DFU with the repo script (builds the release preset first):

```bash
./flash-dual.sh                # build release + flash
PRESET=debug ./flash-dual.sh   # debuggable (-O0) image instead
```

The shared RMCS DFU bootloader must already be on the chip (one-time, needs a
debugger: `./flash-dual-bootloader.sh`). A running app exposes the DFU runtime
interface, so dfu-util detaches it automatically; to force the bootloader to
stay resident, hold the PA07 button through reset.

## Main status LED (RGB)

The 3-channel RGB LED shows the host link and the forwarding buffers. Each
state is a distinct color (yellow = red+green, cyan = green+blue; all three
channels are never lit at once); priority is top to bottom
(`app/src/led/led.hpp`):

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

## CAN status LEDs (light language)

One indicator LED per CAN controller. The on-board silkscreen labels them CAN1
and CAN2, but in firmware they are CAN0/CAN1 (0-based) -- keep the mapping
straight, it is a common source of confusion:

| Silkscreen | Pin  | Firmware | Controller | Bus pins        |
| ---------- | ---- | -------- | ---------- | --------------- |
| CAN1       | PB14 | CAN0     | MCAN0      | TX=PA00 RX=PA01 |
| CAN2       | PB15 | CAN1     | MCAN3      | TX=PA31 RX=PA30 |

The CAN ISR classifies every error interrupt into one of a few states a CAN
controller can actually tell apart, and the LED shows it until ~5 s after the
last error, then returns to off (implemented in `app/src/can/can.hpp` and
`app/src/led/led.hpp`):

| Pattern         | State         | What it means / where to look                                 |
| --------------- | ------------- | ------------------------------------------------------------- |
| off             | healthy       | no errors                                                     |
| slow blink 1 Hz | NO-ACK        | nobody acknowledged -- alone on the bus, partner unpowered, or TX wire cut |
| fast blink 5 Hz | WIRING (Bit0) | bus cannot be driven dominant -- CAN_H/L shorted together, reversed, or open |
| double blink    | SIGNAL        | corrupted bits -- missing 120R termination, baudrate mismatch, or noise |
| solid on        | BUS-OFF       | too many errors; controller is recovering / offline           |

Why only these states: a controller can reliably separate "nobody answered"
(ACK error) from "the bits on the wire are wrong". The exact wire error
(stuff/form/CRC, and the rare Bit1 "stuck dominant") fluctuates frame to frame
and its physical causes (no termination, wrong baudrate, noise) are electrically
indistinguishable, so they are merged into one SIGNAL state instead of pretending
to tell them apart. A hard short/reversal is the one wiring fault that shows up
stably (Bit0), so it gets its own pattern.

Note: CAN errors only arise while the controller is actively transmitting or
receiving. An idle bus reports nothing -- to surface a fault you must send
traffic (e.g. `host/examples/can_error_test`).

### Common mistakes that light the CAN LED

- **Double blink right after another node joins -- duplicate CAN ID.** Two
  transmitters sending the SAME id with different data both win arbitration (the
  id bits are identical), then collide in the data field -> bit error -> SIGNAL.
  Give every transmitter on the bus a unique id. Most frames still get through, so
  the bus looks "mostly working" while the LED stays lit.
- **NO-ACK or double blink against a classic-only device while sending FD.** A
  classic CAN 2.0 node (typical USB-to-CAN adapter, RoboMaster motor) cannot
  acknowledge a CAN-FD frame: it either ignores it (-> NO-ACK) or sends an error
  frame (-> SIGNAL). Send classic frames (`is_fdcan = false`) to such devices.
- **Double blink with no obvious cause -- termination.** A CAN bus needs exactly
  two 120R resistors, one at each end. Zero, one or three cause reflections ->
  intermittent stuff/form/CRC -> SIGNAL.
- **Slow blink with a device attached -- it is not acknowledging.** It is on a
  different bus, unpowered, in listen-only/silent mode, or its TX wire is cut.
- **LED never lights even on a broken bus -- the bus is idle.** Errors only occur
  while transmitting/receiving; send traffic to test.
- **The "wrong" LED reacts -- silkscreen vs firmware numbering.** Board CAN1/CAN2
  are firmware CAN0/CAN1 (see the mapping table above).

### Bus-off auto-recovery

This is a forwarding bridge, so a transient bus fault must not take a CAN port
offline until reboot. When TEC reaches 256 the controller enters bus-off and
hardware latches `CCCR.INIT`, halting it. The ISR clears INIT
(`mcan_enter_normal_mode`) to start the standard bus-off recovery sequence: the
controller waits for the bus to go idle (129 * 11 recessive bits), resets its
error counters and resumes automatically. While the bus stays faulty it simply
cycles bus-off -> recover -> retry, keeping the port alive (and preventing the
TX FIFO from backing up, which would otherwise flash the main LED forever).

## Classic CAN 2.0 vs CAN-FD

Both controllers run permanently in CAN-FD mode (arbitration 1 Mbps, data
5 Mbps). CAN-FD mode is a strict superset: an FD-enabled M_CAN transmits and
receives classic CAN 2.0 frames too, selected per element via the FDF/BRS bits.
So there is no controller-level "mode switch" -- the frame type is chosen
per-frame:

- **Receive**: the hardware auto-detects each frame; `handle_uplink` reports the
  real type back to the host in `CanDataView::is_fdcan`.
- **Transmit**: `handle_downlink` honors the host's per-frame `is_fdcan` flag,
  capped by controller capability: `send_fd = canfd_ && data.is_fdcan`.
  `is_fdcan` defaults to `false`, so a frame is **classic CAN 2.0 unless the host
  explicitly sets `is_fdcan = true`**.

| Host `is_fdcan`     | Controller FD-capable | Frame sent on the wire        |
| ------------------- | --------------------- | ----------------------------- |
| false (default)     | any                   | classic CAN 2.0 (1 Mbps)      |
| true                | yes (this board)      | CAN-FD (1 Mbps arb / 5 Mbps data) |
| true                | no (classic-only)     | classic CAN 2.0 (capped, safe)|

This is free on the hot path: it adds one already-cached bool read and one AND
per frame, and never reconfigures the controller (no INIT-mode stall, no bus
interruption). The host can therefore mix classic and FD frames freely on the
same bus, frame by frame.

Interop note: a classic-only node (e.g. a typical USB-to-CAN adapter, or a
RoboMaster motor) will NOT acknowledge an FD frame -- it ignores it, so the board
sees a pure ACK error and the indicator shows NO-ACK (slow blink). Send classic
frames (`is_fdcan = false`) to such devices. The arbitration baudrate must match
on all nodes regardless (1 Mbps here).
