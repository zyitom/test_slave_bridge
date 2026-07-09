# HPM6E8Y CAN Pin Reverse Engineering

Date: 2026-07-09

This note records the CAN pin scanner result for the recovered board whose schematic is
missing. The test used an external USB-to-CAN adapter that repeatedly transmitted a
standard CAN frame with ID `0x123`. The firmware scanner iterated candidate MCAN pin
muxes; when a candidate RX received `0x123`, the scanner replied through the same
candidate TX.

Scanner response format:

| Byte | Meaning |
| --- | --- |
| 0 | Magic, always `0xC5` |
| 1 | Candidate index |
| 2 | MCAN index |
| 3 | TX bank ASCII |
| 4 | TX pin number |
| 5 | RX bank ASCII |
| 6 | RX pin number |
| 7 | Stage, `0x52` means `R` / received |

Response CAN ID is `0x680 + candidate_index`.

## Confirmed CAN Mapping

| Board silk | Raw response ID | Raw payload | Candidate index | HPM peripheral | TX pin | RX pin | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CAN0 | `0x690` | `C5 10 00 43 00 43 01 52` | `0x10` / 16 | `MCAN0` | `PC00` | `PC01` | RX and TX confirmed |
| CAN1 | `0x689` | `C5 09 01 42 05 42 04 52` | `0x09` / 9 | `MCAN1` | `PB05` | `PB04` | RX and TX confirmed |
| CAN2 | `0x69A` | `C5 1A 02 44 08 44 09 52` | `0x1A` / 26 | `MCAN2` | `PD08` | `PD09` | RX and TX confirmed |
| CAN3 | `0x69B` | `C5 1B 03 44 0F 44 0E 52` | `0x1B` / 27 | `MCAN3` | `PD15` | `PD14` | RX and TX confirmed |

## Analysis

The four physical CAN ports map cleanly to `MCAN0` through `MCAN3`. The board silk order
also matches the peripheral index order, even though the pins are spread across `PB`,
`PC`, and `PD` banks.

These hits are all in the primary A-F IOC bank candidate set, not in the V/W/X/Y/Z
extended candidate set. That makes the result consistent with the smaller BGA package
assumption.

No CAN STB pin was needed for this test. The transceivers appear to be always enabled or
enabled by other board logic.

Against the currently known hpm6e8y board setup, these CAN pins do not collide with:

- USB0 pins used by this board: `PF19`, `PF22`, `PF23`.
- JTAG pins usually seen on the PA bank.
- The currently configured EtherCAT MII pins, which are mainly on `PA`, `PB12`-`PB23`,
  and `PE02`/`PE03`/`PE06`.

The existing bring-up board application still exposes only the old single test CAN
configuration. The normal board port should eventually be updated to use these four CAN
ports:

| Logical CAN | Peripheral | TX | RX |
| --- | --- | --- | --- |
| `can0` | `HPM_MCAN0` | `PC00` | `PC01` |
| `can1` | `HPM_MCAN1` | `PB05` | `PB04` |
| `can2` | `HPM_MCAN2` | `PD08` | `PD09` |
| `can3` | `HPM_MCAN3` | `PD15` | `PD14` |

Implementation notes for the normal firmware:

- Add `clock_can0` through `clock_can3` to the core1 clock group.
- Use `IRQn_MCAN0` through `IRQn_MCAN3`.
- Assign separate MCAN message RAM slices for all four controllers.
- Configure pads as `MCANx_TXD` / `MCANx_RXD` with RX pull-up and hysteresis.
- Keep these pins excluded from later GPIO/LED probing.

## Remaining Board-Recovery Work

- Identify the 17 LED GPIOs.
- Confirm USB HS and JTAG are the EVK-compatible pins assumed so far.
- Confirm EtherCAT port 0/1 MII pins against the actual PHY/magnetics routing.
- Identify the Realtek gigabit Ethernet PHY interface pins separately.

