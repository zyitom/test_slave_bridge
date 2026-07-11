# HPM6E8Y EtherCAT Bridge Bring-up Notes

Date: 2026-07-10

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
