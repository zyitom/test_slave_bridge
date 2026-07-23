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
