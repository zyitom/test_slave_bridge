# ch32_board port - work log

Target: `firmware/ch32_board/` - a 4th firmware target for the WCH **CH32H417**
(dual-core RISC-V, Qingke V5F) whose draw is the **USB 3.0 SuperSpeed** device
controller. All work below is **build-verified only; nothing has run on hardware
yet** (needs a WCH-Link). Source: EVT pack at `~/Downloads/CH32H417EVT`.

## Done

### Board bring-up / build system
- Vendored WCH standard peripheral library into `bsp/wch/` (Core, Peripheral,
  Debug, Startup, Ld) and the CH372Device USBSS stack into `bsp/usb/`.
- CMake build + `cmake/toolchain-wch-riscv.cmake` reusing the **stock
  `riscv32-unknown-elf` GCC** already used for rmcs_board
  (`GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm`). Builds `-march=rv32imafc_zicsr_zifencei
  -mabi=ilp32f` **without** WCH's proprietary `xw` extension, so no MounRiver GCC
  is needed. V5F single core (`-DCore_V5F -DRun_Core=Run_Core_V5F`).
- Strict C11 kept: vendor GNU keywords aliased via `-Dasm=__asm__
  -Dtypeof=__typeof__` (no gnu11). `bsp/syscalls.c` supplies newlib stubs
  (debug.c owns `_write`/`_sbrk`; do NOT add nosys.specs - it collides).
- Build: `cmake --preset debug -S firmware/ch32_board && cmake --build
  firmware/ch32_board/build` -> `ch32_board_app.elf` / `.bin`. ~82% of 128K flash.

### Version management (no git SDK exists)
- WCH ships only a per-chip EVT zip (this one dated 2026.04). Solution =
  vendored in-tree + provenance pin in `bsp/PROVENANCE.md` (source paths,
  per-file versions, re-vendor procedure, and the local-patch list).

### Full forwarding app (CAN + UART + USB SS), canonical style
Mirrors **upstream rmcs_board** (the reference RISC-V board), not mc02:
- `app/src/board_app.hpp/.cpp` - single-board `board::` layer owning GPIO pins +
  clock; `init_can`/`init_uart` return the peripheral clock.
- `app/src/can/` - classic bxCAN. `handle_downlink` transmits **directly** to the
  hardware TX mailboxes (no software TX ring buffer, **no `try_transmit`**);
  `can_array[]` + `HardwareConfig` + `irq_handler()` method + thin ISR shims.
- `app/src/uart/` - WCH USART, interrupt mode (RXNE bytes + IDLE frame delimiter,
  TXE double-buffer TX); `uart_array[]` + `HardwareConfig` + `irq_handler()`;
  `try_transmit()` drains the downlink buffer.
- `app/src/usb/vendor.*` - the librmcs session/keepalive + serializer/deserializer
  logic (board-agnostic, reused from the shared `core/`), with the transport
  swapped to **WCH USBSS bulk EP1** (host has no TinyUSB-SS equivalent).
- `app/src/timer/` - TIM10 (32-bit `CNT_32`) at 1 MHz -> quarter-us timestamps.
- `app/src/led/` - no-op status-LED stub. `app/src/utility/` - `lazy`,
  `ring_buffer`, `interrupt_safe_buffer` (shared), `interrupt_lock` (WCH
  `__disable_irq`), `assert`.
- `app/src/app.cpp` - provides `main()` (replaces the demo `app/User/main.c`,
  excluded from build); WCH clock + `USB_Timer_Init` + `USBSS_Device_Init` +
  range-for peripheral init; run loop matches upstream (usb try_transmit, then
  `for (auto& u : uart_array) u->try_transmit()`).

### USB SS data path (real, not a stub)
- `vendor.cpp` `ss::tx_write` arms EP1 IN (`UEP_TX_DMA/CHAIN_LEN/CHAIN_EXP_NUMP`,
  patterned on the demo's real EP3 arming) with a `tx_in_flight` flag;
  `usb_ss_ep1_in_complete()` / `usb_ss_ep1_out_complete()` service uplink/downlink.
- **Local patch** to vendored `bsp/usb/ch32h417_usbss_it.c`: the EP1 IN+OUT
  `DEF_UEP1` cases had a hardware EP1<->EP2 DMA loopback (the CH372 demo's echo);
  replaced with calls to the two hooks. Marked `LIBRMCS LOCAL PATCH`, recorded in
  `bsp/PROVENANCE.md` (re-apply after any re-vendor). EP1 = bulk pipe, OUT 0x01 /
  IN 0x81 (matches the host SDK's endpoints).

### Host SDK (USB 3.0 SS tuning)
- `host/src/transport/usb/usb.cpp` already uses async libusb (64 in-flight TX
  transfers, `LIBUSB_TRANSFER_FREE_BUFFER`). Bumped `kReceiveTransferCount`
  4 -> 16 (a 4-deep RX pool cannot fill a 5 Gbit pipe). Host builds.

## Key findings / decisions
- **CH32H417 CAN is classic 2.0B, not CAN-FD** - forwarding speed rides entirely
  on USB SS.
- **ITCM/DTCM is free**: the linker runs all code from ITCM (`RAM_CODE=0x200A0000`)
  and puts all `.data`/`.bss` in DTCM (`RAM=0x200C0000`), so mc02's manual
  hot-path placement is unnecessary here.
- Toolchain gotcha: WCH `CANx/USARTx/TIMx` macros are `reinterpret_cast`s (not
  constant expressions) - pass the integer `*_BASE` through `Lazy`'s consteval
  ctor and cast at runtime.
- Our ISRs use plain `__attribute__((interrupt))` (GCC emits `mret`), sidestepping
  the risk that mainline GCC drops WCH's `interrupt("WCH-Interrupt-fast")`.

## Not done / on-target bring-up TODO (all compile-correct, values unconfirmed)
1. **USB SS EP1-OUT received length**: `vendor.cpp` reads `UEP_RX_CHAIN_LEN` and
   the payload from the RX buffer base (single-buffer). Confirm the chained-DMA
   RX length/offset semantics on target for multi-packet bursts.
2. **GPIO pins** (CAN, USART) are placeholders in `board_app.cpp` - set from the
   EVT schematic.
3. **CAN bit timing + timer prescaler** are keyed off `SystemCoreClock` - confirm
   the CAN/TIM kernel-clock dividers.
4. **UART is interrupt-mode**; DMA + idle-line is a deferred optimization (needs
   the DMAMUX request mapping).
5. **Flashing**: EVT uses WCH-Link (wlink / openocd-wch), not the HPM openocd on
   this machine.
6. On-target SS enumeration + speed test; bootloader / DFU.
