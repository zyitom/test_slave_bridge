# HPM6E8Y Ethernet Pin Reverse Engineering

Date: 2026-07-10

This note records the current Ethernet pin recovery state for the board with
missing schematics. The latest correction is that the HPM6E*Y* part has two
on-die 100M Ethernet PHYs for EtherCAT. `PA25` and `PA28` are not board GPIO
yellow LEDs; they are internal PHY LED/address strap pads and must be left to the
PHY.

## Confirmed EtherCAT Facts

For the two on-die 100M PHYs, the external package pins are analog/strap pins,
not parallel MII pins:

- PHY1: `PA16/PA17` RD-/RD+, `PA18/PA19` TD-/TD+, `PA24` RBIAS, `PA25/PA26`
  LED/address strap.
- PHY0: `PA20/PA21` RD-/RD+, `PA22/PA23` TD-/TD+, `PA27` RBIAS, `PA28/PA29`
  LED/address strap.
- Shared management pins: `PA30` MDIO and `PA31` MDC.

The on-die PHY digital side uses internal pads:

- PHY0 MII to ESC0 P0: `PV00`..`PV11`, `PV15`.
- PHY1 MII to ESC0 P1: `PW00`..`PW11`, `PW15`.
- Internal PHY reset inputs: `PV12` and `PW12`, active low, driven as GPIO.
- Internal PHY 25 MHz references: `PW20` and `PW21` as `ESC0_REFCK`.

The probe must first enable the EtherCAT clock, drive the reference clocks, put
the differential/RBIAS/LED0/LDO pads in analog mode, route `PA25`/`PA28` as
`ESC0_CTR_0`/`ESC0_CTR_1` with pull-ups for the LED1/address-strap pads, and
release `PV12`/`PW12`.

After that bring-up, the working management path is:

```text
PA30/PA31 pinmux: ENET0 SMI
PHY ID:           0x937c4024
PHY type:         JL1111-class 100M PHY
```

The ESC MII-management and TSW per-port MDIO access paths on the same pins did
not produce the useful PHY-ID/link results. For this board, keep the internal
PHY pinmux stable and use `ENET0` SMI for PHY management probing.

Normal firmware now follows this model in `board.c`/`board.h`:

- `BOARD_ECAT_DISABLE_ESC_BRINGUP` is no longer defined for `hpm6e8y`.
- `PA16..PA29` are not used as external MII data pins.
- `PV00..PV11/PV15` route ESC0 P0 to PHY0 internally.
- `PW00..PW11/PW15` route ESC0 P1 to PHY1 internally.
- `PV12` and `PW12` are the active-low PHY reset GPIOs.
- `PA25` and `PA28` remain PHY LED/address-strap pads and are also the ESC link
  source controls: `CTR_0` for port1/PHY1 and `CTR_1` for port0/PHY0.
- `BOARD_ECAT_PORT0_PHY_ADDR = 2`, `BOARD_ECAT_PORT1_PHY_ADDR = 1`, matching the
  observed IN/OUT link tests.

## Confirmed RJ45 Mapping

The link tests used one cable at a time and watched the BMSR value from CAN
frames `0x5a0`, `0x5a1`, and `0x5a2`.

| Physical connector | MDIO result |
| --- | --- |
| EtherCAT IN / EtherCAT0 | PHY address `2`, BMSR `0x786d` when linked |
| EtherCAT OUT / EtherCAT1 | PHY address `1`, BMSR `0x786d` when linked |
| Ethernet / Realtek port | No link change on PHY addresses `0`, `1`, or `2` |

Latest operator captures:

| Test | Passing frame | Result |
| --- | --- | --- |
| EtherCAT IN / EtherCAT0 cable inserted | `0x5a2 ... 78 6d` | Confirms PHY address `2` and the IN connector mapping |
| EtherCAT OUT / EtherCAT1 cable inserted | `0x5a1 ... 78 6d` | Confirms PHY address `1` and the OUT connector mapping |
| Realtek Ethernet cable inserted | `0x5e0`/`0x5e1 ... 79 ad`, `0x560`/`0x561 ... 00 1c c9 16` | Confirms Realtek management, PHY power/reset/clock, and external RJ45 link |

With no cable connected, all three reported addresses had BMSR `0x7849`. With
the Ethernet/Realtek connector cabled, all three still stayed at `0x7849`.
Therefore the third RJ45 is not one of the discovered JL1111 EtherCAT PHY
addresses.

PHY address `0` also returned `0x937c4024`, but it did not link with the third
RJ45. Treat it as a JL1111 broadcast/alias response until a broadcast-disable
test proves otherwise. Do not map it to the Realtek Ethernet port.

## FINAL Silkscreen <-> PHY <-> ESC data port (2026-07-11, status-probe)

Confirmed with the `debug-ecat-status-probe` image, one cable at a time, reading
both the per-port PHY BMSR (CAN `0x783`/`0x784`) and the ESC DL Status register
0x0110 (CAN `0x780`, low byte bit4 = ESC port0 physical link, bit5 = ESC port1).
This is the definitive end-to-end map and it is cross-consistent with the
functional test that the PC master only enumerates when plugged into OUT.

| Silkscreen | PHY MDIO addr | probe frame | ESC data port | EtherCAT role | connects to |
| --- | --- | --- | --- | --- | --- |
| EtherCAT0 / IN | 2 | `0x783` (fw calls it "port0") | ESC port1 | downstream | next slave |
| EtherCAT1 / OUT | 1 | `0x784` (fw calls it "port1") | ESC port0 | upstream | PC master |

Raw evidence:

| Cable | `0x783` (addr2) BMSR | `0x784` (addr1) BMSR | `0x780` DL Status | ESC port with link |
| --- | --- | --- | --- | --- |
| EtherCAT0 / IN | `0x786d` link-up | `0x7849` dark | `0x5923` (bit5 set) | ESC port1 |
| EtherCAT1 / OUT | `0x7849` dark | `0x786d` link-up | `0x5613` (bit4 set) | ESC port0 |

Consequences to remember:

- The silkscreen IN/OUT is reversed vs the ESC data path: the connector that
  faces the master (ESC port0, where the PC cable must go) is the one labelled
  **OUT / EtherCAT1**. Plug the master into OUT.
- The firmware macros `BOARD_ECAT_PORT0_PHY_ADDR=2` / `BOARD_ECAT_PORT1_PHY_ADDR=1`
  are MDIO-poll labels only; they are INVERTED relative to the ESC data ports
  (fw "port0"/addr2 is physically ESC port1). This inversion is exactly why
  `BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT=1` is required (addr1 link -> ESC port0
  NMII_LINK, addr2 link -> ESC port1). Do NOT "correct" these labels without
  also removing the swap, or link detection breaks and the master stops
  enumerating.
- Moving the master entry to the IN connector is a PCB-routing change (swap which
  RJ45 differential pair reaches which on-die PHY analog pads); there is no
  `PHY_CFG0`/ESC register that reassigns which PHY feeds which ESC data port.

## Realtek reset pin CONFIRMED: PE01 (active-low PHYRSTB)

The external Realtek RTL8211F is held in reset by default. `PE01` is its
active-low `PHYRSTB`: with it left asserted the PHY reads all `0xffff` on MDIO
and never links (dark RJ45 LEDs); driving `PE01` high releases it and it answers
`0x001cc916` at MDIO address `0` immediately. This was found by the
`realtek_reset_scanner` core1 image (build `debug-realtek-reset-scanner`), which
drives each candidate GPIO low then high and re-reads ENET0 MDIO until the
Realtek OUI appears -- it latched on `PE01` (CAN `0x7f0` payload
`46 45 01 00 00 1c c9 16`).

The earlier probe-v8 capture that saw the Realtek without any firmware releasing
`PE01` was reading leftover state from a prior boot; on a clean power-up nothing
drove `PE01`, so the PHY stayed dark. The RTL8211F runs from its own 25 MHz
crystal, so no SoC reference clock is needed -- reset release alone brings it up.

Firmware must pulse `PE01` (assert low, release high, ~160 ms settle) before any
ENET0 MDIO/RGMII use. `enet_packet_tester` now does this in
`release_realtek_reset()`.

Note: core0's ESC bring-up (`board_init_ethercat` + `ecat_hardware_init`) does NOT
affect the Realtek -- disabling it did not revive the PHY. That path is disabled
for the ENET-probe diagnostic images only to keep them small, not because it
conflicts with the Realtek.

## Realtek Ethernet Search Plan

The external Realtek Ethernet PHY is a separate target. Do not use the EtherCAT
JL1111 result to assign it.

Probe v8 confirmed the Realtek management pins:

```text
PF00 = ETH0_MDC
PF01 = ETH0_MDIO
access path = ENET0 SMI
PHY ID = 0x001cc916
observed responding addresses = 0 and 1
linked BMSR = 0x79ad
```

`0x001cc916` is a Realtek RTL8211-class gigabit PHY ID. `0x79ad` has link
status and auto-negotiation complete set, so the external Ethernet RJ45,
Realtek PHY power, reset, clock, and MDIO/MDC path are alive.

Addresses `0` and `1` both returned the same ID and link status in the first
successful scan. Treat this as an address alias/broadcast/strap ambiguity until
a follow-up test isolates the single real address. It is safe for pin recovery
to say that the management pins are `PF00/PF01`; the final firmware should
choose the address that keeps responding after the Realtek-specific setup is
known.

To identify it:

1. Read the marking on the Realtek chip package and use that exact Realtek
   datasheet pinout. The needed pins are `MDC`, `MDIO`, reset, clock input or
   crystal pins, PHY address straps, RGMII/RMII mode straps, and the RGMII/RMII
   MAC pins.
2. In the HPM6E8Y BGA196 ball table, cross out every candidate RGMII/RMII or
   MDIO/MDC bank that is not bonded out.
3. The MDIO/MDC pins are now known, so the remaining data-pin question is
   whether the Realtek RGMII data bus uses the contiguous `PF02..PF13` group or
   the `PB00..PB11` group.

The probe firmware version `8` adds this first Realtek-focused candidate:

```text
bus 3: PF00/PF01 as ETH0_MDC/ETH0_MDIO, read through ENET0 SMI
status frame: 0x733
basic frames: 0x5e0 + phy_addr
PHY ID frames: 0x560 + phy_addr
hit frames: 0x660 + phy_addr
```

High-value MDIO/MDC candidates to check first:

| Candidate | Why |
| --- | --- |
| `PF00/PF01` | Confirmed Realtek `ETH0_MDC`/`ETH0_MDIO`; scanned as bus `3` in probe v8 |
| `PA30/PA31` | Already proven alive for EtherCAT management, but the Realtek did not show there in the current scan |
| Any MDIO/MDC pair tied to the bonded RGMII bank from the BGA196 table | Final choice must follow the actual bonded ball table and Realtek package pinout |

Remaining non-conflicting ETH0/RGMII candidates from the HPM6E80 iomux:

| Candidate bank | Signals | Conflict status |
| --- | --- | --- |
| `PF00..PF15` | `MDC`, `MDIO`, `TXCK`, `TXD0..3`, `TXEN`, `RXDV`, `RXD0..3`, `RXCK`, `RXER`, `TXER` | Strongest candidate because `PF00/PF01` are confirmed Realtek management pins and the rest is a contiguous bonded ETH0 bank |
| `PB00..PB11` plus `PF00/PF01` | `RXDV`, `RXD0..3`, `RXCK`, `TXCK`, `TXD0..3`, `TXEN`, with management on `PF00/PF01` | Still possible; HPM examples can split management and data banks |
| `PB00..PB11` plus `PA30/PA31` | Same data pins plus `MDIO/MDC` | Unlikely; `PA30/PA31` already read the EtherCAT JL1111 and did not expose the Realtek when the Ethernet RJ45 was cabled |
| `PA16..PA31` / mixed PA bank | RGMII/MII alternate functions exist | Avoid for Realtek; these pads are already confirmed as EtherCAT internal PHY analog/strap/management pins |

For the data pins, look for a complete bonded RGMII/RMII bank connected to the
Realtek package. A gigabit PHY normally needs RGMII, so expect a grouped set like
`TXC`, `TXEN/CTL`, `TXD0..3`, `RXC`, `RXDV/CTL`, and `RXD0..3`, plus
management and reset.

## Realtek RGMII data bus CONFIRMED: PF group (PF02..PF15)

Proven by the ENET packet tester: with `PE01` released, the board transmitted
RGMII frames the PC received intact (source MAC `02:52:4d:43:53:03` = candidate 0
/ PF group, variant 3 = 1000M, TX+RX delay 7/7; payload `RMCS-HPM6E8Y-ENET-PIN-
TEST`, EtherType `0x88b5`). A well-formed frame arriving on the wire proves the
PF RGMII TX pin mapping and timing are correct. Final map:

```text
PF00 MDC   PF01 MDIO
PF02 TXCK  PF03..PF06 TXD0..3  PF07 TXEN
PF08 RXDV  PF09..PF12 RXD0..3  PF13 RXCK
PF14 RXER  PF15 TXER (RTL8211F RGMII does not use ER; harmless)
PE01 PHYRSTB (active-low reset, drive high to release)
```

Working line rate 1000M with tx_delay=7, rx_delay=7 (packet-tester variant 3).

What remains unresolved:

- Realtek data bus: RESOLVED -- PF group (see above). The PB alternative is ruled
  out.
- Realtek true PHY address: first successful scan saw the same `0x001cc916`
  response at addresses `0` and `1`; keep both suspect until broadcast/strap
  behavior is isolated.
- Realtek reset pin: RESOLVED -- `PE01` active-low PHYRSTB (see the confirmed
  section above). Clock is the PHY's own 25 MHz crystal, no SoC clock needed.

## Remaining Closure Tests

EtherCAT pin recovery is complete at the MDIO/link level. The only useful
remaining EtherCAT check is a control test with one cable at a time:

| Cable state | Expected EtherCAT result |
| --- | --- |
| No EtherCAT cable | `0x5a1 ... 78 49` and `0x5a2 ... 78 49` |
| EtherCAT IN only | `0x5a2 ... 78 6d`; `0x5a1` stays `78 49` |
| EtherCAT OUT only | `0x5a1 ... 78 6d`; `0x5a2` stays `78 49` |

The Realtek RGMII data pins cannot be proven by MDIO. MDIO only proves the
management pins and PHY-side link. To close the Realtek data bus, run an ENET0
packet test with the first candidate below, then only try the second if packets
do not pass:

| Candidate | Pin mapping |
| --- | --- |
| `PF00..PF15` | `PF00` MDC, `PF01` MDIO, `PF02` TXCK, `PF03..PF06` TXD0..3, `PF07` TXEN, `PF08` RXDV, `PF09..PF12` RXD0..3, `PF13` RXCK, `PF14` RXER, `PF15` TXER |
| `PB00..PB11` + `PF00/PF01` | `PB00` RXDV, `PB01..PB04` RXD0..3, `PB05` RXCK, `PB06` TXCK, `PB07..PB10` TXD0..3, `PB11` TXEN, with management still on `PF00/PF01` |

A successful packet test means the PC sees raw EtherType `0x88b5` frames from
ENET0 over the Realtek RJ45, and ideally the board also reports matching RX
counters over CAN. Until such a packet test passes, record the Realtek data bus
as `PF02..PF15 likely, not final`.

## ENET Packet Tester Firmware

The packet tester is the data-plane closure step for the external Realtek
Ethernet port. It does not change the normal EtherCAT application. It builds a
temporary core1 image that repeatedly tries the only two useful RGMII pin banks:

| Candidate index | Candidate | Source MAC suffix |
| --- | --- | --- |
| `0` | `PF00..PF15` | `02:52:4d:43:53:00`..`0b` |
| `1` | `PB00..PB11` plus `PF00/PF01` MDIO | `02:52:4d:43:53:10`..`1b` |

For each candidate it also tries four common HPM RGMII delay presets and three
MAC line speeds. This matters because the observed Realtek-like PHY reports link
in BMSR (`0x79ad`) but returns `0x0000` from the RTL8211 PHYSR register used by
the first tester, so the tester must not rely on PHYSR to choose the speed.

| Variant index | TX delay | RX delay | MAC speed |
| --- | --- | --- | --- |
| `0` | `0` | `0` | 1000M |
| `1` | `0` | `7` | 1000M |
| `2` | `7` | `0` | 1000M |
| `3` | `7` | `7` | 1000M |
| `4` | `0` | `0` | 100M |
| `5` | `0` | `7` | 100M |
| `6` | `7` | `0` | 100M |
| `7` | `7` | `7` | 100M |
| `8` | `0` | `0` | 10M |
| `9` | `0` | `7` | 10M |
| `10` | `7` | `0` | 10M |
| `11` | `7` | `7` | 10M |

That means the practical first-pass search is `2 * 12 = 24` variants, not a
random per-pin permutation. If one variant initializes and packets pass, the
RGMII bank is determined. If a bank initializes but packets are unreliable, a
later timing-only sweep can expand the delay search to all `8 * 8` delay values
without changing the pin mapping.

Tester version `3` treats MDIO/link status as advisory. Even if the Realtek PHY
ID read fails after a pinmux change, the tester still forces ENET0 through the
selected speed/delay variant and transmits raw frames. Only ENET
DMA/controller-init failure blocks TX for that variant.

IMPORTANT (regression fixed): the Realtek PHY only links in the same live
on-die PHY environment the MDIO scanner set up. An earlier tester revision only
enabled `clock_gpio/can0/eth0` and muxed the RGMII pins, omitting the ESC core/PHY
clocks, the `PW20`/`PW21` `ESC0_REFCK` reference clocks, and the `PV12`/`PW12`
reset release. In that state the external Realtek stays unclocked -- no link and
dark RJ45 LEDs. The tester now calls the same `release_internal_phys()` bring-up
(ESC/TSN clocks + `esc_core_enable_clock`/`esc_phy_enable_clock` + refclk + reset
release) once at startup before the candidate loop, matching the environment in
which MDIO probe v8 read `0x001cc916` with link BMSR `0x79ad`. If the RJ45 LEDs
are dark, verify this bring-up ran.

Build the packet tester:

```bash
cmake --preset debug-enet-packet-tester -S firmware/rmcs_board/ecat \
    -DBOARD=hpm6e8y
GNURISCV_TOOLCHAIN_PATH=/home/zyi/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux \
    cmake --build firmware/rmcs_board/ecat/build
```

Flash it through DFU:

```bash
dfu-util -d a11c:a904 -e
dfu-util -d a11c:a904 -a 0 \
    -D firmware/rmcs_board/ecat/build/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu \
    -R
```

Connect the PC to the Realtek Ethernet RJ45 and bring the PC interface up. No IP
address is required for this raw-frame test.

On the PC, watch the board transmit raw Ethernet frames:

```bash
sudo ip link set dev <iface> up
sudo tcpdump -i <iface> -e -XX 'ether proto 0x88b5'
```

Expected board frames use source MACs:

| Source MAC pattern | Meaning |
| --- | --- |
| `02:52:4d:43:53:00`..`0b` | PF candidate, variant index `0`..`11` |
| `02:52:4d:43:53:10`..`1b` | PB candidate, variant index `0`..`11` |

Send raw frames from the PC back to the board continuously while the tester
cycles variants:

```bash
sudo python3 - <<'PY'
from scapy.all import Ether, Raw, sendp

iface = "<iface>"
frame = Ether(dst="ff:ff:ff:ff:ff:ff", type=0x88B5) / Raw(b"pc-to-hpm6e8y")
sendp(frame, iface=iface, count=400, inter=0.05, verbose=False)
PY
```

The tester reports over CAN0 (`PC00`/`PC01`) at 1 Mbps classic CAN.

Identity frame:

| CAN ID | Payload |
| --- | --- |
| `0x740` | `45 4e 45 54 03 02 0c 52` |

Status frame:

| CAN ID | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 | Byte 6 | Byte 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x741` | `0x45` (`E`) | candidate | variant index | stage | status | PHY address | selected speed | duplex/link |

Stage is `P` for pins configured, `I` for ENET init result, and `T` while the
variant is transmitting/receiving. Status is `0` for OK, `1` no Realtek PHY,
`2` no link, `3` speed/duplex unresolved, and `4` ENET DMA/controller init
failed. In tester version `3`, status `1`, `2`, or `3` does not block packet TX;
only status `4` does. Selected speed is `1` = 10M, `2` = 100M, `3` = 1000M. Byte
7 bit 0 is link and bit 1 is full duplex.

Counter frame:

| CAN ID | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 | Byte 6 | Byte 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x750` | `0x43` (`C`) | candidate | variant index | TX OK low byte | RX match low byte | TX fail low byte | RX total low byte | link |

PHY frames:

| CAN ID | Meaning |
| --- | --- |
| `0x760 + phy_addr` | Realtek PHY-ID hit, payload bytes 4..7 are ID1/ID2 |
| `0x770` | Selected PHY BMSR/PHYSR and decoded speed/link |

Interpretation:

- If `tcpdump` sees `02:52:4d:43:53:00`..`0b`, the Realtek data bank is `PF`.
- If `tcpdump` sees `02:52:4d:43:53:10`..`1b`, the Realtek data bank is `PB`.
- If CAN `0x750` byte 4 increments while the Scapy sender is running, board RX
  also works for that candidate/delay.
- If status is `4`, the selected pin bank probably does not feed `RXCK` into
  ENET0, so that bank is very likely wrong.

## Historical External-TSW Hypothesis

Earlier notes explored a pure external `TSW0_P1`/`TSW0_P3` explanation. Keep that
as a fallback for any separate external gigabit PHY investigation, but do not use
it to initialize the two EtherCAT 100M PHYs on the HPM6E*Y* package. For
EtherCAT PHY-ID bring-up, the internal-PHY path above is the primary model.

## MDIO Probe Firmware

Build the probe image:

```bash
cmake --preset debug-mdio-pin-scanner -S firmware/rmcs_board/ecat \
    -DBOARD=hpm6e8y
cmake --build firmware/rmcs_board/ecat/build
```

Flash the generated DFU image:

```bash
dfu-util -d a11c:a904 -e
dfu-util -d a11c:a904 -a 0 \
    -D firmware/rmcs_board/ecat/build/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu \
    -R
```

The probe runs on core1 and reports over CAN0 (`MCAN0`, `PC00`/`PC01`) at
1 Mbps classic CAN. It keeps the internal PHY pinmux stable and repeatedly scans
PHY addresses `0..31` through four management-path variants:

- bus `0`: internal on-die PHY bring-up plus ESC MII-management on `PA30`/`PA31`.
- bus `1`: `ENET0` SMI management on `PA30`/`PA31`, with internal PHY pins kept stable.
- bus `2`: `TSW0_P1` MDIO management on `PA30`/`PA31`, with internal PHY pins kept stable.
- bus `3`: `ENET0` SMI management on `PF00`/`PF01`, with internal PHY pins kept stable.

Each scan round starts with an identity frame:

| CAN ID | Payload |
| --- | --- |
| `0x72f` | `4d 44 49 4f 08 04 52 53` |

This decodes as `MDIO`, probe version `8`, and bus count `4`. If this frame is
missing, the new probe firmware is not running or the
CAN capture filter is hiding it.

Status frames:

| CAN ID | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 | Byte 6 | Byte 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x730 + bus` | `0x4D` (`M`) | bus | physical port | stage | hit count | error count | reset drive | access |

Stage is `S` for scan start, `D` for scan done, and `E` if any MDIO transaction
timed out. Access is `C` for the ESC MII-management path, `T` for the TSW
per-port MDIO engine, and `E` for the `ENET0` SMI engine.

Raw read frames:

| CAN ID | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4..5 | Byte 6..7 |
| --- | --- | --- | --- | --- | --- | --- |
| `0x500 + bus * 32 + phy_addr` | `0x52` (`R`) | bus | PHY address | read OK | reg 2 / PHYID1 | reg 3 / PHYID2 |
| `0x580 + bus * 32 + phy_addr` | `0x52` (`R`) | bus | PHY address | read OK | reg 0 / BMCR | reg 1 / BMSR |

PHY hit frames:

| CAN ID | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4..5 | Byte 6..7 |
| --- | --- | --- | --- | --- | --- | --- |
| `0x600 + bus * 32 + phy_addr` | `0x4D` (`M`) | bus | physical port | PHY address | reg 2 / PHYID1 | reg 3 / PHYID2 |

Decode the PHY identity as:

```text
phy_id = (reg2 << 16) | reg3
oui = (reg2 << 6) | ((reg3 >> 10) & 0x3f)
model = (reg3 >> 4) & 0x3f
revision = reg3 & 0x0f
```

Realtek gigabit PHYs commonly show IDs in the `0x001c****` range. The HPM
on-die 100M PHY ID observed on this board is `0x937c4024` through bus `1`
(`ENET0` SMI on `PA30`/`PA31`).

If every bus returns only `0xffff`/`0x0000`, the remaining suspects move from
pinmux to hardware bring-up: VIO_B01 3.3 V, `VDD_PHY0CAP`/`VDD_PHY1CAP` 1.2 V
LDO capacitors, RBIAS resistors, strap levels on `PA25/PA26/PA28/PA29`, or a
missing/blocked internal PHY reset/clock requirement.
