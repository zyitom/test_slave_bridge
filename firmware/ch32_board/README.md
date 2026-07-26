# ch32_board (WCH CH32H417, Qingke V5F) - USB 3.0 SuperSpeed device

Third firmware target in librmcs. Motivation: the CH32H417 has a **USB 3.0
SuperSpeed (5 Gbps)** device controller, which directly attacks the board->host
forwarding ceiling that the FS/HS boards (mc02, c_board) hit.

Status: **enumerates on target as a USB 3.0 SuperSpeed device (5 Gbps)**.
Verified with a WCH-LinkE on 2026-07-25: `1a86`-free identity `A11C:D403`,
`bcdUSB 3.00`, bulk EP `0x01`/`0x81` at 1024 B with `bMaxBurst 15`, stable over
repeated resets. Both cores run: V3F boots, brings up the clock tree and wakes
V5F, which runs the librmcs forwarding app.

The USB 2.0 fallback is deliberately disabled (`LIBRMCS_USBSS_HS_FALLBACK=0`):
the stock stack switches to USB 2.0 ~1.5 s after SuperSpeed training fails, and
USB 2.0's D+/D- are PB8/PB9 - the same pins as SWCLK/SWDIO - so a fallback used
to take the debug interface with it until the next power cycle. Verified on
target: after halting the core for 5 s (which drops the USB device from the
host) the debugger still reconnects.

The bulk data path is verified end to end (2026-07-26): 11/11 downlink packets
parsed and acked over EP1 with no drops. The host SDK reaches the board through
`librmcs::board::Ch32Board` (PID `0xD403`), so the examples see it like any other
board, including runtime UART reconfiguration (`uartN_config`). Still open: the
CAN/UART pin map is placeholder. See "Not done yet".

## UART runtime reconfiguration

A `kUartNConfig` downlink reaches `Uart::handle_config`, which reprograms the
port away from the power-on baudrate in its `kUartPorts` entry. The switch clears
UE before re-running `USART_Init` and re-arms afterwards: `CTLR1_CLEAR_Mask`
(`0x29F3`) preserves UE and the interrupt-enable bits, so TXE has to be disabled
explicitly or the old frame would resume at the new rate. Bytes in flight in
either direction are dropped -- the rate changes mid-character and the peer
cannot be resynchronized -- so quiesce the port before switching.

BRR holds `USARTDIV = HCLK / (16 * baudrate)` as 12.4 fixed point, so only
`HCLK/65536 < baudrate <= HCLK/16` is representable (about 1.5 kBaud .. 6.25
MBaud on a 100 MHz HCLK). Anything else is rejected and the current rate kept,
returning false rather than asserting: this is host-supplied data and a bad
packet must not panic the firmware. Representable is not the same as accurate --
divisor quantization reaches ~4% error at the very top of the range, past what
UART framing tolerates, while standard rates stay well inside it (115200 within
0.006%, 3 MBaud within 1.01%).

## App layout (librmcs C++ layer)

```
app/src/
  app.cpp            # main(): WCH clock + USBSS init + mc02-style forwarding loop
  can/               # WCH classic bxCAN (interrupt mode), can1=CAN1 can2=CAN2
  uart/              # WCH USART (RXNE+IDLE framing, TXE double-buffer TX)
  usb/vendor.*       # CV'd mc02 Vendor: session/serializer/deserialize; WCH SS transport
  timer/             # TIM10 (32-bit), 1 MHz, quarter-us timestamps
  led/               # no-op status-LED stub
  utility/           # lazy / ring_buffer (CV) + WCH interrupt_lock, assert
```

app.cpp provides `main()` and replaces the WCH demo `app/User/main.c`, which is
excluded from the build.

## USB SS data path

The CH372 demo forwards data as a pure hardware DMA loopback (EP1-OUT chains
straight to EP2-IN; the CPU never touches the payload). librmcs needs the CPU to
feed OUT bytes to the deserializer and write serializer batches to IN, so EP1 is
repurposed as the librmcs bulk pipe:
- `usb/vendor.cpp` `ss::tx_write` arms EP1 IN through
  `UEP_TX_DMA / CHAIN_LEN / CHAIN_EXP_NUMP`, gated by a `tx_in_flight` flag that
  the EP1 IN-complete hook clears.
- `bsp/usb/ch32h417_usbss_it.c` is patched (`LIBRMCS LOCAL PATCH`, recorded in
  `bsp/PROVENANCE.md`) so the `DEF_UEP1` IN/OUT cases call
  `usb_ss_ep1_in_complete()` / `usb_ss_ep1_out_complete()` instead of running the
  demo loopback.

Enumeration exercises EP0 only, so this path is **written but not yet proven**.
The first thing to confirm on target is the EP1-OUT received length: the hook
reads `UEP_RX_CHAIN_LEN` and takes the payload from the RX buffer base
(single-buffer, no ping-pong); multi-packet bursts are unverified.

## Layout

```
firmware/ch32_board/
  cmake/toolchain-wch-riscv.cmake  # bare-metal RV32IMAFC/ilp32f, no WCH 'xw'
  cmake/merge_hex.cmake            # V3F@0x0 + V5F@0x10000 -> one .hex
  bsp/wch/                         # vendored WCH std peripheral lib (Core/Peripheral/Debug/Startup/Ld)
  bsp/usb/                         # vendored CH372Device USBSS device stack
  bsp/syscalls.c                   # newlib stubs (debug.c owns _write/_sbrk)
  app/User/                        # system_, ch32h417_it, conf for the V5F app core
  app/src/                         # the librmcs C++ forwarding app (V5F)
  boot/User/                       # system_ for the V3F boot core (owns SystemInit/HSE)
  boot/src/                        # V3F boot + offload core, shared-SRAM mailbox
```

## Build

```bash
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm     # reuses rmcs_board's toolchain
cmake --preset debug -S firmware/ch32_board
cmake --build firmware/ch32_board/build
# -> build/ch32_board_app.elf  (V5F, links at 0x10000)
# -> build/ch32_board_boot.elf (V3F, links at 0x0)
# -> build/ch32_board_merged.hex  <- flash this one
```

Targets: `ch32_board_app`, `ch32_board_boot`, `ch32_board_merged` (all built by
default). `ch32_board_boot` is the V3F boot core image **and** the DFU
bootloader - the two are the same program, see "Bootloader / DFU" below.

## Key porting decisions

- **Standard toolchain, no `xw`.** Qingke V5F implements RV32IMAFC + WCH's
  proprietary `xw` compressed extension. We build `-march=rv32imafc_zicsr_zifencei
  -mabi=ilp32f` *without* `xw`, so the stock upstream GCC already used by
  rmcs_board compiles the whole WCH library. Cost: small code-size increase only.
- **Dual-core, USB on V5F.** `-DRun_Core=Run_Core_V3FandV5F`; two images, merged
  into one flash artifact (`ch32_board_merged.hex`, V3F at `0x0` + V5F at
  `0x10000`). V3F is the boot core: it owns the clock tree (its `SystemInit`
  enables HSE - the V5F `system_ch32h417.c` does **not**) and wakes V5F, then
  runs non-forwarding offload work through a shared-SRAM mailbox. V5F runs the
  forwarding fast path including the USB stack.
  Note this differs from WCH's own dual-core CH372 demo, which runs the USB stack
  on V3F and idles V5F; keeping USB on V5F is what keeps the forwarding hot path
  free of cross-core hops. It works - USB DMA reaches the V5F DTCM buffers fine.
- **Strict C11 kept.** WCH headers use the GNU `asm`/`typeof` keywords; aliased
  via `-Dasm=__asm__ -Dtypeof=__typeof__` instead of switching to gnu11, so the
  repo's "no GNU extensions" policy holds for our own code.

## RESOLVED - interrupt return (`ret` vs `mret`)

This was the bug that kept USB dead. WCH ISRs are declared
`__attribute__((interrupt("WCH-Interrupt-fast")))`. Mainline GCC **rejects that
argument and drops the whole attribute**:

```
warning: argument to 'interrupt' attribute is not '"user"', '"supervisor"', or '"machine"'
```

so `TIM12_IRQHandler`, `USBSS_IRQHandler`, `USBSS_LINK_IRQHandler` and
`USBHS_IRQHandler` compiled as plain functions ending in `ret`. Startup sets
`intsyscr (CSR 0x804) = 0x0F` (HPE hardware stacking + nesting), and returning
without `mret` never restores `mstatus.MIE` nor pops the hardware stack: the
first vendored ISR to fire killed interrupts for good, and the USB link state
machine stalled with the app still happily spinning in its main loop.

Fix, in `CMakeLists.txt`, in the same spirit as the existing `-Dasm=__asm__`:

```cmake
-Dinterrupt\(x\)=interrupt
```

The function-like macro only fires when `interrupt` is followed by `(`, so it
rewrites WCH's `interrupt("WCH-Interrupt-fast")` to the argument-less
`__attribute__((interrupt))` (GCC emits `mret` + register save/restore) and
leaves our own `__attribute__((interrupt))` handlers untouched. Verify after any
toolchain change:

```bash
riscv32-unknown-elf-objdump -d --disassemble=USBSS_LINK_IRQHandler \
    build/ch32_board_app.elf | tail -3    # must end in mret
```

## Flashing (WCH-Link)

```bash
OCD=~/3rd_party/wch-openocd
$OCD/bin/openocd -f $OCD/bin/wch-riscv.cfg \
    -c "init" -c "wch_riscv unfreeze" -c "halt" \
    -c "program firmware/ch32_board/build/ch32_board_merged.hex verify" \
    -c "reset run" -c "exit"
```

- **`wch_riscv unfreeze` must sit between `init` and `halt`.** Move it after `halt`
  and it silently does nothing; `program` then fails with either
  `Read-Protect Status Currently Enabled` or
  `error writing to flash at address 0x00000000`, while `flash erase_sector`
  still reports success and reads come back as `e339e339` (the read-protected
  dummy pattern - genuinely erased flash reads `ffffffff`). MounRiver Studio
  passes exactly `-c init -c "wch_riscv unfreeze"`; its bundled openocd binary
  and `wch-riscv.cfg` are byte-identical to the ones under `~/3rd_party`, so when
  the IDE can flash and the command line cannot, suspect the command sequence,
  not the tool.
- **Do not add `flash erase_sector wch_riscv.flash 0 last`.** That range reaches
  the option bytes. `program` erases what it writes; an extra flash command
  between `unfreeze` and `program` also cancels the unfreeze.
- **Use `wch-riscv.cfg` (single target) to run the board, not
  `wch-dual-core.cfg`.** The dual-core config's `reset run` resumes *both* harts,
  so V5F starts executing the V3F image at flash `0x0` instead of waiting for
  `NVIC_WakeUp_V5F(0x10000)` - the symptom is V5F with `mtvec = 0x20100003`. The
  dual-core config is still the right one for *inspecting* both cores
  (`targets wch_riscv.cpu.0` / `.1`).
- **PC values read after `halt` are not trustworthy** on this target: the WCH
  openocd resets the chip as part of halting, so it repeatedly reads back inside
  the reset-time loadcode copy loop (`0x00004020`). Judge "is the firmware
  running" by USB enumeration, not by PC, and not by `v5f_ready` (V3F zeroes it
  on every boot).
- Halting drops the USB device from the host, and **`resume` does not bring it
  back - only `reset run` does.**

## Bootloader / DFU

On this part the bootloader is not a separate partition: V3F comes out of reset
first, owns the clock tree and decides whether to wake V5F at all, which is
exactly a bootloader's job. So `ch32_board_boot` is both. "Launching the app"
means `NVIC_WakeUp_V5F()`, not a jump, and V3F stays resident afterwards running
the offload loop.

Flash map (`boot/src/flash/layout.hpp`):

```
0x00000 .. 0x10000  bootloader   V3F image, DFU never writes here
0x10000 .. 0x70000  application  V5F image, the only DFU-writable region
0x70000 .. 0x72000  metadata     one erase block: magic + image_size + sha256
```

Boot decision, in `decide_boot_mode()`:

1. boot mailbox holds `DFU0` (the app was asked to detach) -> DFU mode;
2. boot mailbox holds `APP1` (a download just manifested and was hash-checked)
   -> launch, no re-verification;
3. otherwise hash the app slot and compare against the metadata record. A torn
   or absent image means DFU mode, never a launch.

Debugger flashing bypasses DFU, so the build supplies the metadata record
itself: `cmake/gen_metadata_hex.py` hashes `ch32_board_app.bin` and emits the
record at `0x70000` into `ch32_board_merged.hex`. Without it a
debugger-flashed board would cold-boot into DFU every time.

Flashing the app over DFU (no debugger, so the SWD/USB pin conflict below does
not apply):

```bash
./flash-ch32.sh            # from the repo root; PRESET=debug for a -O0 image
# equivalently:
dfu-util -d 0xa11c:0xd403 -a 0 -D firmware/ch32_board/build/ch32_board_app.dfu
```

`ch32_board_app.dfu` is the raw `.bin` plus a DFU suffix - deliberately *not*
the `ImageHash`-suffixed form mc02/c_board use, because this bootloader hashes
what it programmed rather than trusting a suffix. Both images enumerate as
`A11C:D403`; DFU mode is the one whose interface reports `bInterfaceProtocol`
0x02 and whose product string is `RMCS Bootloader v<version>`.

Getting into DFU mode: power up with no valid image, or let `dfu-util` detach a
running app (the application carries a DFU run-time interface on interface 1,
`app/src/usb/dfu_runtime.cpp`). On detach the app drops the SS link, writes the
boot mailbox and parks; the **boot core** performs the reset, because V5F's reset
vector is flash `0x0` - the V3F image - so a self-reset there would run the wrong
core's code (`SharedBlock::reset_request`).

There is no recovery button on this board, so an app that enumerates but ignores
DFU_DETACH still needs the WCH-Link.

## Not done yet

- **On-target DFU run.** The download path is built and the descriptors verified
  in the ELF, but no image has been pushed over DFU on hardware yet. First things
  to watch: the erase/program work happens inside the USBSS interrupt, and the
  V3F image runs from RAM (`Link_v3f.ld` copies it to `0x20100000`), which is
  what makes self-flashing safe - confirm both on target.
- **GPIO pin map** for CAN and USART in `app/src/board_app.cpp` is placeholder;
  set it from the EVT schematic (`EVT/PUB/CH32H417SCH.pdf`).
- **CAN bit timing + timer prescaler** are keyed off `SystemCoreClock`; confirm
  the CAN/TIM kernel-clock dividers on target.
- SS speed test (WCH ships a host speed-test tool with the CH372 demo).
