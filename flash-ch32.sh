#!/usr/bin/env bash
#
# Flash the ch32_board application over USB DFU.
#
# Prerequisite: the V3F boot core image (which IS the DFU bootloader on this
# part) is already in flash. It is only reachable with a WCH-LinkE, using the
# archived WCH OpenOCD fork (a mainline openocd cannot drive this part -- see
# firmware/ch32_board/tools/openocd-wch/README.md):
#
#   OCD=firmware/ch32_board/tools/openocd-wch
#   $OCD/bin/openocd -f $OCD/bin/wch-riscv.cfg \
#       -c "init" -c "wch_riscv unfreeze" -c "halt" \
#       -c "program firmware/ch32_board/build/ch32_board_merged.hex verify" \
#       -c "reset run" -c "exit"
#
# After that, use this script: DFU needs no debugger, which matters more here
# than on the other boards -- the USB 3 connector's USB 2.0 D+/D- share pins with
# SWCLK/SWDIO, so debugger flashing and a plugged USB cable are mutually
# exclusive (firmware/ch32_board/PITFALLS.md).
#
# Get the board into DFU mode first, either of:
#   - power up with no valid app image in flash (the bootloader stays in DFU), or
#   - let dfu-util detach a running app: the application advertises a DFU
#     run-time interface, reboots into the bootloader on DFU_DETACH, and comes
#     back as a DFU-mode device.
#
# Usage:
#   ./flash-ch32.sh                 # builds the release preset, then flashes
#   PRESET=debug ./flash-ch32.sh    # build/flash a debuggable (-O0) image instead
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/firmware/ch32_board/build"
DFU_IMAGE="$BUILD_DIR/ch32_board_app.dfu"
DFU_ID="0xa11c:0xd403" # VENDOR_ID:PRODUCT_ID, the same pair in app and bootloader

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found (need >= 0.11)" >&2
    exit 1
}

: "${GNURISCV_TOOLCHAIN_PATH:?set it to the RISC-V toolchain root, e.g. ~/3rd_party/hpm}"

# Build the exact image about to be flashed. Flashing is deployment, so this
# defaults to the release preset (optimized + NDEBUG). The presets share one
# build dir, so switching PRESET triggers a full reconfigure.
PRESET="${PRESET:-release}"
echo ">> Building ch32_board_app (preset: $PRESET)"
cmake --preset "$PRESET" -S "$SCRIPT_DIR/firmware/ch32_board"
cmake --build "$BUILD_DIR" --target ch32_board_app

if [[ ! -f "$DFU_IMAGE" ]]; then
    echo "error: $DFU_IMAGE not found after the build (is dfu-suffix installed?)." >&2
    exit 1
fi

echo ">> ch32_board app image: $DFU_IMAGE"
echo ">> DFU devices currently visible:"
dfu-util -l 2>/dev/null | grep -i "a11c:d403" ||
    echo "   (none in DFU mode yet; dfu-util will try to detach a running app)"

# -a 0 selects the only interface the bootloader exposes (the application slot).
dfu-util -d "$DFU_ID" -a 0 -D "$DFU_IMAGE"

echo ">> Done. The bootloader hashes what it programmed, commits the metadata"
echo "   record and resets; the next boot wakes V5F into the new app."
echo "   (A 'lost device' / status-read error from dfu-util at the end is normal.)"
