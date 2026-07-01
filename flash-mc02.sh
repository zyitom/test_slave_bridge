#!/usr/bin/env bash
#
# Flash the mc02 application over USB DFU.
#
# Prerequisite: the mc02 DFU bootloader is already on the board (flashed once via
# SWD/J-Link, see README "烧录 Bootloader"). Put the device into DFU mode first:
#   - power up with no valid app in flash (bootloader stays in DFU), or
#   - trigger a DFU reboot from the host while the app runs.
# A running app also exposes the DFU runtime interface, so dfu-util will detach
# and re-enumerate it into DFU mode automatically.
#
# Usage:
#   ./flash-mc02.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DFU_IMAGE="$SCRIPT_DIR/firmware/mc02/build/app/mc02_app.dfu"
DFU_ID="0xa11c:0xd402" # VENDOR_ID:PRODUCT_ID baked into the .dfu suffix (alt 0 = Internal Flash)

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found (need >= 0.11)" >&2
    exit 1
}

if [[ ! -f "$DFU_IMAGE" ]]; then
    echo "error: $DFU_IMAGE not found." >&2
    echo "build it first:  cmake --build firmware/mc02/build --target mc02_app" >&2
    exit 1
fi

echo ">> mc02 app image: $DFU_IMAGE"
echo ">> DFU devices currently visible:"
dfu-util -l 2>/dev/null | grep -i "a11c:d402" || \
    echo "   (none in DFU mode yet; dfu-util will try to detach a running app)"

# -d filters to the mc02 VID:PID, -a 0 selects the Internal Flash alt setting.
dfu-util -d "$DFU_ID" -a 0 -D "$DFU_IMAGE"

echo ">> Done. The bootloader verifies the image and resets into the app."
echo "   (A 'lost device' / status-read error from dfu-util at the end is normal.)"
