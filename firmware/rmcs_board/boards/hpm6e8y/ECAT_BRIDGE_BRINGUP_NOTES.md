# HPM6E8Y EtherCAT Bridge Bring-up Notes

Updated: 2026-07-13

This note records the traps found while bringing up the `hpm6e8y` EtherCAT
bridge on hardware without a schematic. Keep it close to the board port: several
of these facts contradict the HPM6E00 EVK-style assumptions that were present at
the start of the investigation.

## Current Status

The clean normal firmware is able to run the EtherCAT bridge stably:

```text
EtherCAT link up on "enxc84d4429a4d3": slave "ECAT_Device", 48B chunks, expected WKC 3
EtherCAT bridge connected on enxc84d4429a4d3, session established.
EtherCAT cycle rate: 12.1-12.2 kHz (0 wkc errors)
EtherCAT link closing: 364428 cycles total, 0 wkc errors
```

This means the EtherCAT physical link, ESC, PDO mapping, WKC, and protocol
session are working.

### 2026-07-13 IgH loss and latency validation

Two host-side protocol bugs previously looked like transport or dual-core loss:

1. The CAN deserializer exposed a payload span backed by its pending-byte
   cache, then reused that cache while reading the following hardware
   timestamp. When both pieces crossed input chunks, the timestamp replaced
   the first four payload bytes. Captured bad values decoded exactly as board
   uptime in microseconds. Payload and timestamp are now read with one
   contiguous `peek_bytes()` window.
2. The host treated every libusb receive completion as a complete protocol
   datagram and discarded a field split at that boundary. USB bulk completions
   are arbitrary slices of a reliable byte stream, especially with the
   EtherCAT bridge's cross-core-ring shuttle. USB and EtherCAT reception now
   retain partial deserializer state across callbacks.

The USB transmit pool also applies backpressure when all 64 asynchronous
transfers are in use. It no longer returns an empty buffer and silently omits a
packet under load.

Hardware results with CAN0 connected to CAN1 and CAN2 connected to CAN3, CAN-FD
at 1 Mbit/s arbitration and 5 Mbit/s data phase:

```text
IgH, 12k frame/s/stream, 30 s:
  352907 sent / 352907 received on each stream
  gap=0 corrupt=0 reorder=0
  1122973 EtherCAT cycles, 0 WKC errors

IgH, 16k target frame/s/stream, 20 s:
  312145 sent / 312145 received on each stream (~15.6k actual)
  gap=0 corrupt=0 reorder=0
  739822 EtherCAT cycles, 0 WKC errors

IgH with SCHED_FIFO, 16k frame/s/stream, 12 s:
  192000 sent / 192000 received on each stream
  gap=0 corrupt=0 reorder=0, 0 WKC errors

IgH with SCHED_FIFO, 18k frame/s/stream, 12 s:
  216003 sent / 216003 received on each stream
  gap=0 corrupt=0 reorder=0, 0 WKC errors

USB, 16k target frame/s/stream, 20 s:
  239359 sent / 239359 received on each stream (~12.0k actual)
  gap=0 corrupt=0 reorder=0
  80642 pacing slots skipped by non-realtime host scheduling

USB with SCHED_FIFO, 18k frame/s/stream, 10 s:
  180001 sent / 180001 received on each stream
  gap=0 corrupt=0 reorder=0

USB and IgH with SCHED_FIFO, 20k target frame/s/stream:
  CAN0 plateaued near 19.4k frame/s
  CAN2 plateaued near 19.8k frame/s
  EtherCAT still reported 0 WKC errors and both transports reported 0 corruption
```

`missed pacing slots` are frames the stress generator never submitted after a
scheduler delay or transport backpressure. They are not packets lost after
submission; compare the explicit sent/received counters for that.

The nearly identical 20k USB and IgH plateaus identify the physical CAN/TX
path as the sustained-rate limit. A 20k command period is 50 us, while the
observed frames occupy about 51.5 us on CAN0 and 50.5 us on CAN2 (the CAN ID and
payload affect bit stuffing). Once the 32-element nonblocking MCAN TX FIFO is
full, the firmware reports `downlink_buffer_full` and drops that command;
EtherCAT and USB cannot add capacity to the CAN wire.

The four-byte receive timestamp is not the sustained-rate bottleneck in the
measured two-stream setup. A standard 8-byte CAN uplink field is 15 bytes with a
timestamp and 11 bytes without one. At two 18k streams, timestamped uplink
traffic is only 0.54 MB/s. Removing the timestamp reduces byte load by 26.7%
and increases byte headroom by 36.4%, but it does not raise the approximately
19.4-19.8k physical CAN ceiling.

Do not extend that conclusion unchanged to four synchronized buses. Four
timestamped fields occupy 60 bytes and therefore cross two 44-byte ARQ payload
chunks; four fields without timestamps occupy exactly 44 bytes and fit in one.
At four times 18k frame/s, the byte rates are 1.08 MB/s with timestamps and
0.792 MB/s without them. The former has almost no margin against the measured
full-duplex IgH byte-echo rate below. Thus timestamps are not the present
two-bus loss cause, but removing or making them optional is useful if all four
buses must run near 18k simultaneously. The gross PDO budget
(`44 bytes * approximately 38 kHz = approximately 1.67 MB/s`) is not the same
as sustained application-stream throughput.

One 18k run showed 21-22 missing uplink fields near a five-second cycle-rate
log, but a fresh 12-second run crossed two log points with `216003/216003` per
stream. The transient has not been reproduced, so it cannot be attributed to
logging as a confirmed root cause. Synchronous formatting and output still do
not belong in a production real-time cycle thread; publish counters and log
them from a lower-priority thread instead.

Queue-free CAN0-to-CAN1 RTT with one CAN-FD frame in flight, the transport
thread on isolated CPU 7 at `SCHED_FIFO 80`, and the sender on CPU 6 at
`SCHED_FIFO 70`:

```text
USB (50000 samples): p50 125.0 us, p99 152.2 us, p99.9 209.5 us,
  max 1111.1 us, 0 timeouts
IgH (50000 samples): p50 129.8 us, p99 140.6 us, p99.9 150.9 us,
  max 291.9 us, 0 timeouts, 0 WKC errors
```

Configure the sender's CPU affinity and `SCHED_FIFO` policy only after the board
object has been constructed. Doing it before construction makes the SDK's
250 ms keepalive thread inherit the sender's CPU and FIFO priority. The
busy-waiting sender then starves that thread, the firmware's 1000 ms session
lease expires after approximately 7.5k queue-free frames, and every later CAN
command is ignored even though EtherCAT continues with zero WKC errors. This
was a host benchmark scheduling bug, not CAN or EtherCAT loss.

The measured IgH cycle rate was approximately 37 kHz, or a 27 us process-data
period. The full CAN loopback RTT is several such scheduling/transport stages
plus CAN wire time; moving ESC/SSC to core1 may reduce internal handoff cost,
but it was not the cause of the observed corruption or loss.

### 2026-07-13 transport-only USB versus IgH comparison

The core1 byte-echo validation image isolates the host transport, core0 shuttle,
cross-core rings and core1 handoff from the external CAN wiring. The same host
tool and CPU placement were used for both modes: the I/O thread ran on CPU 7 at
`SCHED_FIFO 80`, the sender was pinned to CPU 6, and one frame was in flight for
the latency runs.

```text
44-byte frame (one ARQ payload chunk):
  USB, 20 s: 233850/233850, corrupt 0
    RTT p50 80.1 us, p99 110.9 us, p99.9 123.8 us, max 1019.7 us
  IgH, 20 s: 252574/252574, corrupt 0, 0 WKC errors
    RTT p50 78.2 us, p99 87.6 us, p99.9 109.6 us, max 135.3 us

64-byte frame (two ARQ payload chunks):
  USB, 30 s: 350541/350541, corrupt 0, RTT p50 80.1 us, p99 112.1 us
  IgH, 30 s: 236916/236916, corrupt 0, 0 WKC errors,
    RTT p50 131.8 us, p99 137.0 us

44-byte frame, 64 in flight, 10 s:
  USB: 360819/360819, 1550.4 KiB/s per direction, corrupt 0
  IgH: 247928/247928, 1065.3 KiB/s per direction, corrupt 0, 0 WKC errors
```

The 44-byte result is the relevant single-PDO case: USB and IgH have essentially
the same median, while IgH has the tighter tail. Crossing from 44 to 64 bytes
adds approximately 54 us to the IgH median because the frame needs a second
ARQ chunk; USB is not quantized by the EtherCAT PDO size. The transport-only
results also survived USB to IgH to USB ownership changes with no reset and no
missing bytes.

An absolute one-way number cannot be recovered from these software timestamps:
the host steady clock and the board clock are not synchronized. Under the
explicit assumption that downlink and uplink are symmetric, half of the
44-byte RTT gives a median one-way estimate of 40.1 us for USB and 39.1 us for
IgH; the p99 estimates are 55.5 us and 43.8 us respectively. Use synchronized
hardware clocks or a scope/logic analyzer for ground-truth one-way latency.

The USB runtime product string is `RMCS EtherCAT Bridge v<version>`, not
`RMCS Agent v<version>`. The host scanner now accepts that exact identity only
for HPM6E8Y PID `0xA904`, while still requiring the exact protocol version.
USB stress and latency tools no longer use `dangerously_skip_version_checks`.
After restoring the normal core1 image, the default USB API completed the real
protocol session handshake, confirming both cross-core directions were live.

`done: 0 CAN frames, 0 UART bytes received` from `ecat_board_test` does not mean
EtherCAT failed. That example sends CAN0 and UART0 traffic and counts what the
board receives back. CAN frames are counted only if an external CAN node sends
frames to the board, and UART bytes are counted only if the UART is looped back
or connected to a responder.

## Clean Normal Firmware

The normal bridge firmware should be built with all diagnostic core1 modes off:

```bash
GNURISCV_TOOLCHAIN_PATH="$HOME/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux" \
cmake --build firmware/rmcs_board/ecat/build-ecat-normal-debug --target rmcs_ecat_core0
```

Flash it with:

```bash
sudo dfu-util -d 0xa11c:0xa904 -a 0 \
  -D firmware/rmcs_board/ecat/build-ecat-normal-debug/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

Run the host test with:

```bash
sudo ip link set dev enxc84d4429a4d3 up
sudo ./host/build/examples/ecat_board_test enxc84d4429a4d3 30
```

Expected healthy EtherCAT result: one slave named `ECAT_Device`, expected WKC 3,
and zero WKC errors over the run.

## Major Traps

### 1. HPM6E8Y uses on-die EtherCAT PHYs

Do not copy the HPM6E00 EVK external MII pin assumptions onto this board.

The HPM6E*Y* package has two on-die 100M EtherCAT PHYs. The external PA pads are
mostly analog/strap pins:

- PHY differential/RBIAS/strap pads live around `PA16..PA29`.
- Shared management is `PA30` MDIO and `PA31` MDC.
- The ESC digital MII side uses internal pads:
  - `PV00..PV11/PV15`
  - `PW00..PW11/PW15`
- PHY resets are internal GPIO pads `PV12` and `PW12`, active low.
- `PW20` and `PW21` provide the internal PHY reference clocks.

Do not mux `PA16..PA29` as external ESC MII data pins.

### 2. PA25 and PA28 are not normal yellow LEDs

`PA25` and `PA28` were first found during LED probing, but they are actually
internal PHY LED/address-strap pads. Treat them as PHY strap/link-source related
pads, not as free GPIO status LEDs.

Normal firmware parks only safe LEDs. The EtherCAT RUN/ERR LEDs are routed to
`PC20` and `PC21` through `ESC0_CTR_2` and `ESC0_CTR_3`.

An unlit red ERR LED is not itself a failure. During a healthy INIT/OP path with
no AL error, the error LED may be off.

### 3. ENET0 SMI is the proven internal PHY management path

The reliable way to read the two internal JL1111-class PHYs is:

```text
PA30/PA31 muxed as ENET0 MDIO/MDC
access path: ENET0 SMI
PHY ID:      0x937c4024
```

The firmware temporarily muxes `PA30/PA31` to ENET0 SMI for PHY reads, then
restores them to ESC MDIO/MDC.

### 4. The third RJ45 is not EtherCAT

The Realtek RTL8211F Ethernet port is a separate RJ45 and a separate problem:

```text
PF00/PF01 = ENET0 MDC/MDIO
PE01      = RTL8211F PHYRSTB, active low
PF02..PF15 = confirmed RGMII data bus
```

Do not use the Realtek RJ45 for EtherCAT tests. EtherCAT tests must use the two
JL1111 on-die PHY RJ45s.

### 5. Physical RJ45 to PHY address mapping

The hardware/user-facing names are valid: the RJ45 marked EtherCAT0 is the
physical EtherCAT IN connector, and the RJ45 marked EtherCAT1 is the physical
EtherCAT OUT connector.

Status-probe confirmation with only EtherCAT0/IN cabled:

```text
0x783 = 50 42 00 03 31 00 78 6d  # port0 / PHY addr 2: read OK + link up
0x784 = 50 42 01 01 31 00 78 49  # port1 / PHY addr 1: read OK + link down
0x782 = 47 50 00 00 10 10 03 00  # GPR_CFG2 = 0x10100000, swapped link mapping
```

This confirms EtherCAT0/IN is PHY address `2`. It does not remove the firmware
requirement to swap that physical link into ESC logical port 1, documented below.

Observed one-cable-at-a-time mapping:

| Physical connector | MDIO PHY address | Linked BMSR |
| --- | --- | --- |
| EtherCAT0 / physical IN label | `2` | `0x786d` |
| EtherCAT1 / physical OUT label | `1` | `0x786d` |

No cable gives BMSR `0x7849`.

### 6. ESC logical port mapping is reversed from the physical labels

This was the root cause of the original:

```text
error: No EtherCAT slave found on the network
```

When the EtherCAT0 / physical IN connector was linked, PHY address `2` reported
link up. Feeding that link to ESC logical port 0 produced no discoverable slave.
Feeding it to ESC logical port 1 made SOEM enumerate `ECAT_Device` immediately.

The important observed values:

```text
Bad mapping:
  0x782 = 47 50 00 00 00 11 03 00
  GPR_CFG2 = 0x11000000
  result: No EtherCAT slave found

Working mapping:
  0x782 = 47 50 00 00 10 10 03 00
  GPR_CFG2 = 0x10100000
  result: ECAT_Device found, expected WKC 3
```

Keep the software link mapping swapped unless hardware evidence proves a better
fix. In current code this is represented by:

```c
#define BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT (1)
```

Operational meaning:

```text
physical EtherCAT0 / PHY addr 2 -> ESC logical port 1
physical EtherCAT1 / PHY addr 1 -> ESC logical port 0
```

This is why the two RJ45s appear reversed from the physical IN/OUT labels. A
single-slave test works in this state. Before changing this for a chained
topology, test both physical connectors explicitly and record the desired
logical topology.

A direct no-swap test was run with:

```c
#define BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT (0)
```

Result: the host returned `No EtherCAT slave found on the network`. Restoring
`BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT` to `1` restored enumeration and session
establishment. Therefore the production hpm6e8y bridge firmware should keep
`swap = 1` while still documenting EtherCAT0 as the physical IN connector.

### 7. Use software GPR link state, not a LINK input pad

The on-die PHYs do not provide the same stable external LINK pin path as the
EVK's external PHYs. The SDK default of sourcing `NMII_LINKx` from IO is not
usable here.

The firmware drives ESC link state through `GPR_CFG2`:

- `NMII_LINKx_FROM_IO = 0`
- `NMII_LINKx_GPR = 0` means link valid
- `NMII_LINKx_GPR = 1` means link invalid

Do not force both ports up. With one cable connected, a false-up empty port can
prevent the ESC from closing the loop and the master may not receive frames.

Normal firmware polls both PHY BMSR link bits every 100 ms and refreshes the GPR
state. A startup-only poll leaves the ESC with stale link state when the cable is
inserted after boot or after a transient disconnect, making recovery appear to
require a power cycle.

### 8. Force 100M MII mode at startup

The clean normal firmware explicitly configures:

- ESC `PHY_CFG0.MAC_SPEED = 1` for 100M
- ESC `PORT0_RMII_EN/PORT1_RMII_EN/PORT2_RMII_EN = 0`
- JL1111 page 7 register 16 bit 3 cleared, selecting MII

Do this once after the EtherCAT port layer releases PHY reset. Avoid repeatedly
writing PHY page registers while the bridge is running.

### 9. Diagnostic CAN and normal CAN0

Diagnostic images used MCAN0 on `PC00/PC01` to report bring-up frames such as
`0x77f`, `0x780`..`0x789`, `0x78a`, and `0x78b`.

The normal EtherCAT bridge application now uses the same physical CAN0 pins, but
without the diagnostic telemetry task. Its logical host `CAN0` is:

```text
logical CAN0 = physical silk CAN0 = HPM_MCAN0
TX/RX        = PC00/PC01
baud         = classic CAN 1 Mbit/s
```

Therefore:

- Seeing diagnostic frames on MCAN0/PC00-PC01 proves the physical CAN0 path.
- The clean normal bridge does not send `0x77f/0x78x` diagnostic frames.
- `ecat_board_test` sends CAN ID `0x123` on logical CAN0 = MCAN0/PC00-PC01.
- A USB-CAN adapter in listen-only mode may not ACK board TX frames.
- The host example only counts received CAN frames when something sends frames
  back to the board. It does not count the board's own transmitted frames.

To validate CAN0:

1. Connect to physical CAN0, wired to MCAN0/PC00-PC01.
2. Use classic CAN 1 Mbit/s.
3. Ensure the other node ACKs frames. Do not use listen-only mode for this test.
4. To make `ecat_board_test` print `[CAN0 RX]`, transmit any classic CAN frame
   from the external node to the board while the test is running.

### 10. UART0 echo requires a physical loopback

The host example sends UART0 text on:

```text
logical UART0 = HPM_UART1
TX/RX         = PY07/PY06
baud          = 921600
```

`done: 0 UART bytes received` is expected unless `PY07` is looped to `PY06` or a
device sends UART data back.

### 11. Diagnostic v4 was useful but not clean normal firmware

The v4 diagnostic firmware proved the port mapping by sending:

```text
0x77f = ESTA 04
0x782 = GPR_CFG2 + MII_MNG_CS
0x783/0x784 = PHY BMCR/BMSR
0x785/0x786 = PHY IDs
0x787/0x788/0x789 = ESC config/status
0x78a/0x78b = JL1111 page7 RMSR
```

It also helped expose that continuous diagnostic CAN/PHY access is not suitable
for the clean normal bridge. The clean normal build removes the diagnostic CAN
task and avoids periodic PHY page writes.

## Symptom Map

| Symptom | Meaning | Action |
| --- | --- | --- |
| `No EtherCAT slave found` | Link was being reported to the wrong ESC logical port | Keep swapped PHY-link-to-ESC-port mapping |
| `ECAT_Device`, WKC 3, 0 WKC errors | EtherCAT transport is healthy | Proceed to CAN/UART physical validation |
| `SAFE-OP+ERROR 0x001B` | SyncManager watchdog | Remove diagnostic runtime interference first; then inspect SSC/SM if it persists |
| No `0x77f/0x78x` frames on clean normal | Expected | Diagnostic CAN is disabled in normal bridge firmware |
| `done: 0 CAN frames` | No CAN frames received from an external node | Check MCAN0/PC00-PC01, 1 Mbit classic, ACK, and external transmit |
| `done: 0 UART bytes` | No UART loopback/responder | Jumper PY07 to PY06 or connect a responder |

## Known Open Items

- Physical connector labels appear reversed relative to ESC logical port order:
  physical EtherCAT0/IN is currently ESC logical port 1, physical EtherCAT1/OUT
  is ESC logical port 0. This is documented and working for a single-slave link,
  but chained topologies should be tested before changing code.
- The normal bridge currently exposes one logical CAN bus: physical CAN0
  (MCAN0/PC00-PC01). The remaining confirmed physical CAN1..CAN3 ports are not
  exposed by `RmcsBoardEcatBridge` yet.
