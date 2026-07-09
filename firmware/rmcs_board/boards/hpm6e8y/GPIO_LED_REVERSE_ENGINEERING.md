# HPM6E8Y GPIO LED reverse engineering

This board (BGA192 hpm6e8y with the on-die PHY) has 17 active-high LED GPIO
outputs, but the schematic/pinout is missing. The LED scanner image drives each
candidate GPIO high one at a time and reports the current candidate over the
already confirmed CAN0 port.

## Scan policy: exclude only proven-fixed pads

The 1000M Ethernet port and the EtherCAT in/out ports exist on this board but
their pinout is NOT confirmed, so their pads are deliberately left IN the scan --
any of the 17 LEDs could sit on a pad we would otherwise wrongly attribute to the
network wiring. The candidate table is generated in `app/led_pin_scanner.cpp`
from all six GPIO banks (A-F, 32 pads each = 192 pads) minus only the pads whose
function is genuinely proven on this board:

- UART0 console (on-board FT2232 debug bridge): PA00, PA01
- JTAG: PA04, PA05, PA06, PA07, PA08
- CAN0: PC00, PC01 -- CAN1: PB04, PB05 -- CAN2: PD08, PD09 -- CAN3: PD14, PD15
- USB0 ID/OC/PWR: PF19, PF22, PF23
- XPI0 boot NOR flash (CS0/SCLK/DQS/D0-D3): PB25-PB31. core0 runs XIP from this
  flash, so re-muxing any of these to GPIO hard-faults core0 mid-scan. PB24 is
  only the unused second chip-select (CA_CS1) and stays scannable (it is the
  KEYA user key input on this board).

Everything else -- including all of bank E, the ESC/RGMII candidate groups, and
pads this BGA192 package may not even bond out -- is scanned. Driving an unbonded
or dedicated pad is harmless: it simply never lights a LED. This is the safe
direction of error, because an over-eager exclusion could permanently hide a real
LED, while an extra harmless candidate only costs a little scan time.

### Always-on domain (banks X / Y / Z)

The board's RGB status LED does NOT live in banks A-F: it sits in the always-on
domain next to MCAN4 (PZ00/PZ01) and UART1 (PY06/PY07), which is why a bare A-F
sweep leaves three LEDs lit at power-up and can never turn them off. The scanner
therefore also covers the always-on pads, which need one extra routing write to
reach the SoC GPIO (bank X is plain IOC, bank Y routes through PIOC, bank Z
through BIOC, all via ALT function 3):

- Bank X: PX00-PX07 (all eight)
- Bank Y: PY02, PY03, PY04 only -- PY00/PY01 (power-domain UART) and PY05
  (`PWDG_RSTN` watchdog reset) are unsafe to toggle blind; PY06/PY07 are UART1
- Bank Z: PZ03-PZ07 -- PZ00-PZ02 are MCAN4 TXD/RXD/STBY. PZ03-PZ07 are the most
  likely RGB channels.

The resulting set has 183 candidates (167 in A-F plus 16 always-on). Do NOT trust
any earlier RGB/EtherCAT LED pin guesses in `board.h` / `app/board_app.hpp`;
those were unverified and are exactly what this scan is meant to replace.

## Build and flash

```bash
cmake -S firmware/rmcs_board/ecat -B firmware/rmcs_board/ecat/build_led_scan -G Ninja \
    -DBOARD=hpm6e8y \
    -DRMCS_ECAT_CORE1_LED_PIN_SCANNER=ON \
    -DRMCS_ECAT_CORE1_CAN_PIN_SCANNER=OFF \
    -DRMCS_ECAT_CORE1_LOOPBACK=OFF
cmake --build firmware/rmcs_board/ecat/build_led_scan
```

Flash this DFU artifact through the existing bootloader:

```bash
dfu-util -d a11c:a904 -a 0 -D firmware/rmcs_board/ecat/build_led_scan/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

## How to record LED pins

Each candidate is exercised in two windows so both kinds of LED are catchable:
a ~700 ms HIGH window (stage `H`) reveals a LED that is normally OFF and turns
ON, and a ~500 ms LOW window (stage `L`) reveals a LED that sits HIGH at reset
(there are a few of these -- likely the RGB channels -- because firmware does not
park the LED pads, so they glow until the scan drives their pad low). Watch for
BOTH a LED switching on during `H` and a stuck-on LED switching off during `L`.

1. Connect USB2CAN to the confirmed CAN0 connector, 1 Mbps classic CAN.
2. Start a CAN log before powering or resetting the board.
3. The scanner sends standard IDs `0x700 + index` throughout each candidate.
4. When a physical LED turns ON, record the latest `H` frame; when a stuck-on LED
   turns OFF, record the latest `L` frame. Either way `byte2`/`byte3` is the pad.
5. Decode payload:

```text
byte0 = 0x4c ('L')
byte1 = candidate index
byte2 = GPIO bank ASCII, for example 0x45 = 'E'
byte3 = GPIO pin number
byte4 = stage: 'B' begin, 'H' driven high, 'L' driven low
byte5 = candidate count, currently 183
```

The CAN standard id equals `0x700 + candidate index`, and `byte2`/`byte3` carry
the bank ASCII and pin number of the pad being driven, so you can identify the
pad from either the id or the payload.

Stop once 17 unique LEDs have been observed. Those 17 bank/pin pairs are the board LED GPIO pinout.

## Scan results (decoded 2026-07-09)

All 15 GPIO LEDs are CONFIRMED (operator verified the physical LEDs against the
confirmation image's blink order, 2026-07-10). Every LED is in banks A/B/C/E --
NONE in the always-on X/Y/Z domain, so the earlier "RGB lives in the PZ domain"
guess was wrong; the RGB is in bank E. board.c `board_park_leds_off()` now drives
all 15 to OFF at boot (RGB active-low high=off; indicators active-high low=off),
fixing the EtherCAT0 yellow power-up glow.

Polarity is NOT uniform: the main RGB is **active-LOW** (common-anode) and so is
EtherCAT0 yellow (PA25); the other 11 indicator LEDs are **active-HIGH**. See the
polarity column per LED. The scan's `H`/`L` stage on its own does not
prove polarity (a stuck-on active-low LED also changes state during the `H`
window); the RGB polarity was pinned down separately -- the original firmware left
green (PE04) glowing precisely because it pulled that pad low, which only lights an
active-low LED.

| LED (physical label)   | Pad  | id     | polarity   | note            |
|------------------------|------|--------|------------|-----------------|
| Main RGB - red         | PE05 | 0x76f  | active-low | lit at power-up |
| Main RGB - green       | PE04 | 0x76e  | active-low | lit at power-up |
| Main RGB - blue        | PE03 | 0x76d  | active-low | lit at power-up |
| CAN0 - green           | PC26 | 0x748  | active-high|                 |
| CAN0 - blue            | PC27 | 0x749  | active-high|                 |
| CAN1 - green           | PE00 | 0x76a  | active-high|                 |
| CAN1 - blue            | PE02 | 0x76c  | active-high|                 |
| CAN2 - green           | PA09 | 0x702  | active-high|                 |
| CAN2 - blue            | PB00 | 0x719  | active-high|                 |
| CAN3 - green           | PB02 | 0x71b  | active-high|                 |
| CAN3 - blue            | PB03 | 0x71c  | active-high|                 |
| EtherCAT port0 - yellow| PA25 | 0x712  | active-low | OFF = drive high |
| EtherCAT port1 - yellow| PA28 | 0x715  | active-high|                 |
| EtherCAT middle - green| PC20 | 0x742  | active-high|                 |
| EtherCAT middle - red  | PC21 | 0x743  | active-high|                 |

This is the complete set: 15 GPIO LEDs (the operator confirmed there are exactly
15 controllable ones; the two ENET link LEDs are PHY-driven, see below).

**PA25 (EtherCAT0 yellow) is a normal active-low LED** (OFF = drive the pad high),
same polarity as the main RGB. It blinks cleanly. An earlier suspicion that it
latched via the PHY was wrong: the confirm image had it flagged active-high, so
its per-blink OFF step drove the pad LOW (= on) and left it lit when the sweep
advanced. With the correct active-low flag it ends OFF. `board_park_leds_off()`
drives it high (off) at boot.

Originally three LEDs were lit at power-up: Main blue PE03, Main green PE04 (both
active-low, whose pads sit low at reset), and EtherCAT0 yellow PA25. With the RGB
parked high, PA25 held high, and the ESC bring-up disabled, none glow at power-up.

### These results contradicted several assumed pin assignments (now resolved)

- `board.h`/`board_app.hpp` RGB guess was PE14/PE15/PE04. Actual RGB is
  PE05(R)/PE04(G)/PE03(B), active-LOW. FIXED: board_app.hpp pins + polarity,
  board.c `board_turnoff_rgb_led()` now drives them high (off).
- `board_app.cpp` `init_can_indicator_pins()` said "No per-CAN indicator LEDs."
  Wrong: every CAN port has green+blue indicators (active-high). Comment
  corrected; the pins are not wired up as indicators yet.
- `board.c` `init_esc_pins()` (EVK-copied) put ESC functions on real LED pads:
  PA09=REFCK is CAN2-green, PA25=TXD_0 is EtherCAT0-yellow, PA28=TXD_3 is
  EtherCAT1-yellow, PE02=CTR_6 is CAN1-blue, PE03=CTR_1 is Main-blue. A pad cannot
  be both an MII line and a static GPIO LED, so this pinmux cannot work here and it
  re-grabbed the LED pads after they were parked (Main blue kept glowing).
  RESOLVED for now: PE02/PE03 removed from init_esc_pins, and the whole ESC
  bring-up is compiled out via `BOARD_ECAT_DISABLE_ESC_BRINGUP` (board.h). Core0
  then only runs the USB DFU runtime; core1 CAN/UART fieldbus is unaffected. Set
  that macro to 0 and supply the real EtherCAT pinout to re-enable.

### Two non-GPIO LEDs

The board also has ENET 1000M and 100M link-status LEDs. These are driven by the
Ethernet PHY's own LED output pins (hardware link/speed indication), not by MCU
GPIO, so the scanner cannot light them. They are not part of the 15 GPIO LEDs.

## LED confirmation image

Once the 15-LED map above is known, this image verifies it: on start it drives
every known LED to OFF (honouring polarity, which also parks the EtherCAT0 pad
that otherwise floats high), then blinks each LED twice in turn. Before each LED
it sends a CAN0 announce frame so you can match the blinking physical LED to its
pad.

```bash
cmake -S firmware/rmcs_board/ecat -B firmware/rmcs_board/ecat/build_led_confirm -G Ninja \
    -DBOARD=hpm6e8y -DRMCS_ECAT_CORE1_LED_CONFIRM=ON
cmake --build firmware/rmcs_board/ecat/build_led_confirm
dfu-util -d a11c:a904 -a 0 -D firmware/rmcs_board/ecat/build_led_confirm/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

Announce frame (std id `0x700 + led index`, order = the table above):

```text
byte0 = 0x43 ('C')
byte1 = led index (0..14)
byte2 = GPIO bank ASCII
byte3 = GPIO pin number
byte4 = polarity: 'H' active-high, 'L' active-low
byte5 = led count (15)
```

## USB runtime DFU note

The EtherCAT core0 app now enumerates a TinyUSB DFU-runtime interface with VID:PID `a11c:a904`. After flashing the app, `lsusb` should still show the board in runtime mode. A DFU detach request from runtime writes the same boot mailbox used by the normal app and resets into the bootloader DFU mode.

### Known issue (to fix later): DFU-runtime detach dies after running a while

`dfu-util -d a11c:a904` sometimes fails with `Failed to retrieve language
identifiers` / `Cannot set alternate interface zero: LIBUSB_ERROR_OTHER` once the
board has been running. The device still enumerates (visible in `lsusb`) but stops
answering USB control transfers because core0 stopped servicing `tud_task()`
(`usb_runtime.cpp`) -- in the old ESC-bridge path core0 returns from `main()` when
`ecat_hardware_init` fails, or the SSC `MainLoop` stalls. Disabling the ESC
bring-up (core0 now loops `tud_task()` forever) should make it reliable, but this
still wants verification/hardening.

Recovery when it happens: hold the KEYA button (PB24) while resetting/power-cycling
-- the bootloader (`bootloader/src/main.cpp`, `board_check_bootloader_force_stay_requested`)
then stays in its own DFU mode, independent of the wedged app, and
`dfu-util -d a11c:a904 -a 0 -D <image>` writes directly.
