# ch32_board BSP provenance

WCH does **not** ship a git-based SDK for the CH32H417. The only distribution is
the per-chip **EVT** ("Evaluation board ToolKit") zip from <https://wch-ic.com>.
There is no upstream repository to submodule (unlike `hpm_sdk`, which is the
`zyitom/hpm_sdk` fork submodule), so the WCH standard peripheral library is
**vendored in-tree** under `bsp/wch/` and pinned here.

Treat `bsp/wch/` and `bsp/usb/` as third-party: do not hand-edit except to
re-vendor a newer EVT (see procedure below). Our own code lives in `app/` and
`bsp/syscalls.c`.

## Source

| | |
|---|---|
| Package | CH32H417EVT |
| Publisher | Nanjing Qinheng Microelectronics (WCH) |
| Package date | 2026.04 |
| Local copy | `~/Downloads/CH32H417EVT/EVT` |
| Target core | Qingke V5F (RV32IMAFC + WCH `xw`, built without `xw`) |

## Vendored trees and their EVT source paths

| In-repo path | EVT source path |
|---|---|
| `bsp/wch/Core/` | `EVT/EXAM/SRC/Core/` |
| `bsp/wch/Peripheral/` | `EVT/EXAM/SRC/Peripheral/` |
| `bsp/wch/Debug/` | `EVT/EXAM/SRC/Debug/` |
| `bsp/wch/Startup/` | `EVT/EXAM/SRC/Startup/` |
| `bsp/wch/Ld/` | `EVT/EXAM/SRC/Ld/` |
| `bsp/usb/` | `EVT/EXAM/USBSS/DEVICE/CH372Device/Common/` |
| `app/User/` (seed) | `EVT/EXAM/USBSS/DEVICE/CH372Device/V5F/User/` |

## Pinned file versions (from each file's WCH header)

Core/Startup:
- `core_riscv.c` V1.0.2 (2026/02/02), `core_riscv.h` V1.0.2 (2026/03/18)
- `startup_ch32h417_v5f.S` V1.0.3 (2025/11/20)
- `debug.c` / `debug.h` V1.0.1

Peripheral drivers currently relied on (full list in the tree):
- `ch32h417_can.c` V1.0.2 (2025/10/21)   - classic bxCAN 2.0B, NOT CAN-FD
- `ch32h417_usart.c` V1.0.1 (2025/09/12)
- `ch32h417_rcc.c` V1.0.3 (2026/03/17)
- `ch32h417_gpio.c` V1.0.1, `ch32h417_dma.c` V1.0.0, `ch32h417_tim.c` V1.0.0
- `ch32h417_flash.c` V1.0.2 (2026/04/08)

USBSS stack (`bsp/usb/`): from CH372Device demo, `ch32h417_usbss_device.*`
V1.0.1 (2026/04/10).

## Local patches (must be re-applied after re-vendoring)

`bsp/usb/ch32h417_usbss_it.c` - EP1 repurposed as the librmcs bulk pipe. The
CH372 demo's EP1<->EP2 hardware DMA loopback (in the `USBSS_UIF_TRANSFER` handler,
`DEF_UEP1` cases for both IN and OUT) is replaced by calls to
`usb_ss_ep1_in_complete()` / `usb_ss_ep1_out_complete()`, which live in
`app/src/usb/vendor.cpp`. Search the file for `LIBRMCS LOCAL PATCH`. When
re-vendoring, re-apply these two case-body replacements plus the two `extern`
declarations near the top.

`bsp/usb/usb_desc.h` / `bsp/usb/usb_desc.c` - librmcs device identity. Three
edits, all marked `LIBRMCS LOCAL PATCH`:

1. `usb_desc.h`: `DEF_USB_VID` / `DEF_USB_PID` / `DEF_USB30_PID` changed from the
   CH372 demo's `0x1A86:0x5537` to `0xA11C:0xD403` (librmcs VID, ch32_board type
   PID; c_board is `0xD401`, mc02 `0xD402`).
2. `usb_desc.h`: `MyManuInfo` / `MyProdInfo` / `MySerNumInfo` lose their `const`
   and gain a `librmcs_usb_init_descriptors()` declaration.
3. `usb_desc.c`: the definitions of those three string descriptors are deleted.
   They now live in `app/src/usb/descriptors.cpp`, which builds them at runtime -
   the product string has to carry `LIBRMCS_PROJECT_VERSION_STRING` (the host SDK
   checks it) and the serial is derived from the chip signature words.

4. `usb_desc.c`: `SS_ConfigDescriptor` rebuilt. The demo advertised one interface
   with six endpoints (three bulk pairs); librmcs uses a single pair, so EP2/EP3
   and their companion descriptors are deleted (`bNumEndpoints` 6 -> 2) and a DFU
   run-time interface is appended (`bNumInterfaces` 1 -> 2, `wTotalLength` 0x60 ->
   0x3E). The DFU interface is what `app/src/usb/dfu_runtime.cpp` answers class
   requests for; its `iInterface` is 0 because this stack only serves string
   indices 0..3 plus the OS string. Layout mirrors mc02's `TUD_DFU_RT_DESCRIPTOR`.

5. `usb_desc.c`: `SS_ConfigDescriptor` exists twice, selected by
   `LIBRMCS_DFU_DEVICE` (`CMakeLists.txt`: 0 for `ch32_board_app`, 1 for
   `ch32_board_boot`). The `#if` branch is the bootloader's DFU-mode
   configuration - one interface, no endpoints, `bInterfaceProtocol` 0x02 and a
   DFU functional descriptor with `wTransferSize` 512 (== `DEF_USBSSD_UEP0_SIZE`,
   so a DNLOAD block is exactly one control-OUT packet). The `#else` branch is
   the application configuration described in 4. Both images share the same
   VID/PID; the product string and this protocol byte are what tell them apart.

Everything else in `usb_desc.c` (device / BOS / HS / FS descriptors) is
unmodified WCH. Note the HS/FS configuration descriptors still describe the demo
layout: with `LIBRMCS_USBSS_HS_FALLBACK=0` they are unreachable, and the DFU
build does not override them either.

`bsp/usb/ch32h417_usbss_it.c` + `ch32h417_usbss_device.c` + `ch32h417_usbss_device.h`
- EP2 and EP3 removed entirely. The demo kept two extra bulk pairs enabled in
`UEP_TX_EN`/`UEP_RX_EN`, armed in `USBSS_Device_Endp_Init()`, and serviced by
loopback code in the ISR - including an EP2-OUT case that armed `EP1_TX` from
EP2's receive registers, i.e. stray host traffic on EP2 could inject into the
librmcs uplink. Deleted: the two `USBSS_EP{2,3}_Rx_Buf` definitions and externs
(64 KB of SRAM), `EP2_Chain_Sel` / `EP3_T_Chain_Sel` / `EP3_R_Chain_Sel`, the
EP2/EP3 arming in `USBSS_Device_Endp_Init()`, and the four EP2/EP3 ISR cases
(which now stall through `default`). Paired with the `SS_ConfigDescriptor` edit
above - the descriptor and the hardware enable must agree.

`bsp/usb/ch32h417_usbss_device.c` + `bsp/usb/ch32h417_usbhs_device.c` - no USB 2.0
fallback. Two guards on `LIBRMCS_USBSS_HS_FALLBACK` (defined in `CMakeLists.txt`,
0 by default), both marked `LIBRMCS LOCAL PATCH`:

1. `USB_Timer_Start()` returns early on `ENABLE`, so TIM12 - which exists only to
   time out SuperSpeed link training and switch to USB 2.0 - is never armed.
2. `USBHS_Device_Init()` returns early on `ENABLE`, catching the paths where the
   USBSS link handler reaches for USB 2.0 directly (`LINK_STATE_INACTIVE`,
   `U2U3_SUCC`) without going through TIM12. Disable paths stay live.

Reason: USB 2.0's D+/D- are PB8/PB9, which on this package are also SWCLK/SWDIO.
A fallback hands those pins to the USB2 PHY and the debug interface drops until
the next power cycle - which happens on every USB disconnect, including the one
caused by halting the core in a debugger. Set the macro to 1 to restore stock
behaviour and USB 2.0 host support.

## Re-vendoring procedure (when a newer EVT is released)

1. Download the new `CH32H417EVT` zip from wch-ic.com; unzip to a scratch dir.
2. Re-copy the trees per the table above **over** `bsp/wch/` and `bsp/usb/`.
   Do NOT copy `bsp/syscalls.c` or anything under `app/` (those are ours).
3. `git diff -- firmware/ch32_board/bsp` and review: WCH bumps individual file
   versions independently, so most files stay byte-identical. Pay attention to
   `core_riscv.*`, `startup_*.S`, `Ld/V5F/Link_v5f.ld`, and any peripheral we
   drive (can/usart/dma/gpio/rcc/tim/usbss).
4. Update the version table above from the new file headers.
5. Rebuild; re-check the interrupt-return risk documented in `README.md` if
   `startup_*.S` or the USBSS IT files changed.
