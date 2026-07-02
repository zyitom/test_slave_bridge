#!/usr/bin/env bash
#
# Flash the rmcs_board hpm5321_dual_can application over USB DFU.
#
# The HPM5321 board carries the same "RMCS DFU Bootloader" as mc02/c_board, so the
# app is flashed with dfu-util -- no debugger (J-Link/OpenOCD) needed. A debugger
# is only required to put the DFU bootloader onto a blank chip the first time
# (see flash-dual-bootloader.sh / README).
#
# Prerequisite: the DFU bootloader is already on the board. Put the device into
# DFU mode first:
#   - power up with no valid app (bootloader stays in DFU), or
#   - trigger a DFU reboot from the host while the app runs.
# A running app exposes the DFU runtime interface, so dfu-util will detach and
# re-enumerate it into DFU mode automatically.
#
# Usage:
#   ./flash-dual.sh                 # builds the release preset, then flashes
#   PRESET=debug ./flash-dual.sh    # build/flash a debuggable (-O0) image instead
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DFU_IMAGE="$SCRIPT_DIR/firmware/rmcs_board/build/app/output/rmcs_board_app_hpm5321_dual_can.dfu"
DFU_ID="0xa11c:0xa902" # VENDOR_ID:PRODUCT_ID baked into the .dfu suffix (alt 0 = Internal Flash)

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found (need >= 0.11)" >&2
    exit 1
}

# Build the exact image about to be flashed. Flashing is deployment, so this
# defaults to the release preset (optimized + NDEBUG). The presets share one
# build dir, so switching PRESET triggers a full reconfigure.
PRESET="${PRESET:-release}"
: "${GNURISCV_TOOLCHAIN_PATH:?GNURISCV_TOOLCHAIN_PATH must point to the RISC-V toolchain root}"
echo ">> Building rmcs_board_app (preset: $PRESET, BOARD=hpm5321_dual_can)"
cmake --preset "$PRESET" -S "$SCRIPT_DIR/firmware/rmcs_board" -DBOARD=hpm5321_dual_can
cmake --build "$SCRIPT_DIR/firmware/rmcs_board/build" --target rmcs_board_app

if [[ ! -f "$DFU_IMAGE" ]]; then
    echo "error: $DFU_IMAGE not found after the build." >&2
    exit 1
fi

echo ">> hpm5321_dual_can app image: $DFU_IMAGE"
echo ">> DFU devices currently visible:"
dfu-util -l 2>/dev/null | grep -i "a11c:a902" || \
    echo "   (none in DFU mode yet; dfu-util will try to detach a running app)"

# -d filters to the dual-can VID:PID, -a 0 selects the Internal Flash alt setting.
dfu-util -d "$DFU_ID" -a 0 -D "$DFU_IMAGE"

echo ">> Done. The bootloader verifies the image and resets into the app."
echo "   (A 'lost device' / status-read error from dfu-util at the end is normal.)"
