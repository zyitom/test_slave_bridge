# ch32_board (WCH CH32H417, Qingke V5F) - USB 3.0 SuperSpeed device

Third firmware target in librmcs. Motivation: the CH32H417 has a **USB 3.0
SuperSpeed (5 Gbps)** device controller, which directly attacks the board->host
forwarding ceiling that the FS/HS boards (mc02, c_board) hit.

Status: **full forwarding app (CAN + UART + USB SS) builds** under the repo's
unified CMake + the stock `riscv32-unknown-elf` toolchain, mirroring the mc02
architecture. Not yet flashed / enumerated on target. The transport drivers are
compile-correct first cuts; the USB SS data path in particular is a placeholder
(see "OPEN: USB SS data path" below). This is a compiles-clean scaffold pending
on-board bring-up.

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

## OPEN: USB SS data path (the real bring-up gap)

The CH372 demo forwards data as a pure hardware DMA loopback (EP1-OUT chains
straight to EP2-IN; the CPU never touches the payload). librmcs needs the CPU to
feed OUT bytes to the deserializer and write serializer batches to IN. So:
- `usb/vendor.cpp` `ss::tx_ready/tx_write/tx_write_zlp` are compile-correct
  PLACEHOLDERS - they stage data and gate on `USBSS_DevEnumStatus` but do not yet
  arm the real EP1 IN chain.
- The EP1-OUT completion in `bsp/usb/ch32h417_usbss_it.c` must call
  `usb_ss_deliver_downlink(data, len)` (provided in vendor.cpp) instead of the
  demo loopback.
Wiring these against the USBSS chained-DMA registers is the main on-target task.

## Layout

```
firmware/ch32_board/
  cmake/toolchain-wch-riscv.cmake  # bare-metal RV32IMAFC/ilp32f, no WCH 'xw'
  bsp/wch/                         # vendored WCH std peripheral lib (Core/Peripheral/Debug/Startup/Ld)
  bsp/usb/                         # vendored CH372Device USBSS device stack
  bsp/syscalls.c                   # newlib stubs (debug.c owns _write/_sbrk)
  app/User/                        # main.c, system_, ch32h417_it, conf (V5F single-core)
```

## Build

```bash
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm     # reuses rmcs_board's toolchain
cmake --preset debug -S firmware/ch32_board
cmake --build firmware/ch32_board/build
# -> build/ch32_board_app.elf + .bin
```

## Key porting decisions

- **Standard toolchain, no `xw`.** Qingke V5F implements RV32IMAFC + WCH's
  proprietary `xw` compressed extension. We build `-march=rv32imafc_zicsr_zifencei
  -mabi=ilp32f` *without* `xw`, so the stock upstream GCC already used by
  rmcs_board compiles the whole WCH library. Cost: small code-size increase only.
- **V5F single-core only.** `-DCore_V5F -DRun_Core=Run_Core_V5F`. The dual-core
  V3F+V5F HSEM bring-up is deferred; V5F runs the USB stack directly.
- **Strict C11 kept.** WCH headers use the GNU `asm`/`typeof` keywords; aliased
  via `-Dasm=__asm__ -Dtypeof=__typeof__` instead of switching to gnu11, so the
  repo's "no GNU extensions" policy holds for our own code.

## OPEN RISK - interrupt return (must validate on hardware)

WCH ISRs are declared `__attribute__((interrupt("WCH-Interrupt-fast")))`.
Mainline GCC 13 **rejects that argument and drops the attribute**, compiling the
handlers as plain functions that return with `ret`, not `mret`.

Startup sets `intsyscr (CSR 0x804) = 0x0F`, enabling HPE hardware stacking +
nesting - the mode `WCH-Interrupt-fast` is built for. WCH's hardware stack
push/pop is paired with `mret`; a plain `ret` epilogue likely will NOT trigger
the hardware pop, corrupting context on IRQ exit (USBSS_IRQHandler,
USBSS_LINK_IRQHandler, TIM12_IRQHandler, USBHS_IRQHandler).

Recommended fix once hardware is available:
- Change these handlers to plain `__attribute__((interrupt))` (GCC emits `mret`
  + register save/restore) - always correct, HPE just becomes redundant; **or**
- Build the ISR translation units with the WCH MounRiver GCC (which honors
  `WCH-Interrupt-fast`) while keeping the rest on the stock toolchain.

## Not done yet

- Flashing path: EVT flashes via **WCH-Link** (wlink / openocd-wch), not the HPM
  openocd on this machine. Need WCH-Link hardware + tooling.
- On-target SS enumeration + speed test (WCH provides a host speed-test tool).
- librmcs protocol / telemetry integration (bulk EP <-> core protocol).
- Host SDK: confirm it can drive USB 3.0 SS bulk transfers.
- Bootloader / DFU story (other boards ship app + bootloader).
