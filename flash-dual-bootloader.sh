#!/usr/bin/env bash
#
# Flash the rmcs_board hpm5321_dual_can DFU BOOTLOADER via J-Link (one-time).
#
# This is only needed for a blank chip or to recover/update the bootloader. After
# the bootloader is on the board, flash the application with ./flash-dual.sh over
# USB DFU -- no debugger required for the app.
#
# How the J-Link is used:
#   The HPM5321 boots from XIP QSPI NOR, whose programming needs a flash algorithm
#   that knows the NOR controller. The HPM SDK ships that algorithm for *OpenOCD*
#   (the hpm_xpi flash driver), and OpenOCD drives your J-Link over JTAG. So this
#   script calls OpenOCD with the jlink probe -- you don't run OpenOCD by hand, and
#   J-Link is just the physical adapter. (OpenOCD is already installed; override
#   with OPENOCD=/path if needed.) Standalone JLinkExe/J-Flash is NOT used because
#   the SDK has no Segger flash loader for this part.
#
# Wiring: connect the J-Link to the board's JTAG header (TCK/TMS/TDI/TDO/nRST/GND).
#
# Usage:
#   ./flash-dual-bootloader.sh
#   OPENOCD=/path/to/openocd ./flash-dual-bootloader.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BL_BIN="$SCRIPT_DIR/firmware/rmcs_board/build/bootloader/output/rmcs_board_bootloader_hpm5321_dual_can.bin"

HPM_SDK_BASE="$SCRIPT_DIR/firmware/rmcs_board/bsp/hpm_sdk"
OPENOCD_DIR="$HPM_SDK_BASE/boards/openocd"
OPENOCD_BIN="${OPENOCD:-openocd}"

command -v "$OPENOCD_BIN" >/dev/null 2>&1 || {
    echo "error: openocd not found. It drives the J-Link here." >&2
    echo "       set OPENOCD=/path/to/openocd (e.g. the one shipped with the HPM toolchain)." >&2
    exit 1
}
if [[ ! -f "$BL_BIN" ]]; then
    echo "error: $BL_BIN not found." >&2
    echo "build it first:  cmake --build firmware/rmcs_board/build" >&2
    exit 1
fi

echo ">> Flashing DFU bootloader @ 0x80000000 via J-Link (JTAG): $BL_BIN"
echo "   (BOARD=hpm5300evk supplies the xpi0 NOR flash bank; same SoC/flash as this board.)"

# PROBE jlink -> SDK sources interface/jlink.cfg with transport=jtag.
# BOARD hpm5300evk -> defines 'flash bank xpi0 hpm_xpi 0x80000000 ...'.
"$OPENOCD_BIN" -s "$OPENOCD_DIR" \
    -c "set HPM_SDK_BASE $HPM_SDK_BASE; set BOARD hpm5300evk; set PROBE jlink;" \
    -f hpm5300_all_in_one.cfg \
    -c "program $BL_BIN 0x80000000 verify reset exit"

echo ">> Done. Now flash the app over USB DFU:  ./flash-dual.sh"
