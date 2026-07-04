#!/usr/bin/env bash
#
# Flash the rmcs_ecat_bridge (hpm6e80ivm1) application over USB DFU.
#
# The single .dfu file contains BOTH cores: core1's program is embedded in the
# core0 image as a C array and loaded into core1's ILM at boot, so the dual-core
# chip flashes exactly like the single-core boards. Prerequisite: the DFU
# bootloader is on the board (one-time, see ./flash-ecat-bootloader.sh). Put the
# device into DFU mode by powering up with no valid app, or by holding the user
# key KEYA (PB24) through reset; a running app that exposes the DFU runtime
# interface is detached automatically.
#
# Usage:
#   ./flash-ecat.sh                   # builds the release preset, then flashes
#   PRESET=debug ./flash-ecat.sh      # debuggable (-O0/-Og) image -- NOTE: the
#                                     # PDI ISR and SSC stack get several times
#                                     # slower; never measure latency on debug
#   LOOPBACK=1 ./flash-ecat.sh        # P1 bring-up image (core1 = lossless echo,
#                                     # pairs with host/build/examples/ecat_stream_latency)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ECAT_DIR="$SCRIPT_DIR/firmware/rmcs_board/ecat"
DFU_IMAGE="$ECAT_DIR/build/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e80ivm1.dfu"
DFU_ID="0xa11c:0xa903" # VENDOR_ID:PRODUCT_ID baked into the .dfu suffix (alt 0 = Internal Flash)

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found (need >= 0.11)" >&2
    exit 1
}

PRESET="${PRESET:-release}"
: "${GNURISCV_TOOLCHAIN_PATH:?GNURISCV_TOOLCHAIN_PATH must point to the RISC-V toolchain root}"
LOOPBACK_FLAG="OFF"
[[ "${LOOPBACK:-0}" != "0" ]] && LOOPBACK_FLAG="ON"

echo ">> Building rmcs_ecat core1+core0 (preset: $PRESET, loopback: $LOOPBACK_FLAG)"
cmake --preset "$PRESET" -S "$ECAT_DIR" -DRMCS_ECAT_CORE1_LOOPBACK="$LOOPBACK_FLAG"
# Force a core1 relink so the embedded sec_core_img.c always matches the
# requested variant (the emit step only runs when core1 itself relinks).
touch "$ECAT_DIR/core1/src/main.cpp"
cmake --build "$ECAT_DIR/build" --target rmcs_ecat_core0

if [[ ! -f "$DFU_IMAGE" ]]; then
    echo "error: $DFU_IMAGE not found after the build." >&2
    exit 1
fi

echo ">> ecat bridge image (core0 + embedded core1): $DFU_IMAGE"
echo ">> DFU devices currently visible:"
dfu-util -l 2>/dev/null | grep -i "a11c:a903" || \
    echo "   (none in DFU mode yet; dfu-util will try to detach a running app)"

echo ">> Flashing needs root to access the DFU USB device; you may be asked for your password."
sudo dfu-util -d "$DFU_ID" -a 0 -D "$DFU_IMAGE"

echo ">> Done. The bootloader verifies the image and resets into the app."
echo "   (A 'lost device' / status-read error from dfu-util at the end is normal.)"
