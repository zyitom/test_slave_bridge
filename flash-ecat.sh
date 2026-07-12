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
# The image name and its baked-in USB id both track the selected board
# (rmcs_ecat_bridge_<board>.dfu, PID per boards/<board>/CMakeLists.txt), so
# derive both from the produced file AFTER the build rather than hard-coding a
# board -- otherwise switching BOARD in the CMake cache silently breaks this.
OUTPUT_DIR="$ECAT_DIR/build/rmcs_ecat_core0/output"

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found (need >= 0.11)" >&2
    exit 1
}

PRESET="${PRESET:-release}"
# Default to the known local toolchain install when the env var is not set.
DEFAULT_TOOLCHAIN="$HOME/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux"
if [[ -z "${GNURISCV_TOOLCHAIN_PATH:-}" && -d "$DEFAULT_TOOLCHAIN" ]]; then
    export GNURISCV_TOOLCHAIN_PATH="$DEFAULT_TOOLCHAIN"
fi
: "${GNURISCV_TOOLCHAIN_PATH:?GNURISCV_TOOLCHAIN_PATH must point to the RISC-V toolchain root}"
# Always pass the flag explicitly so a previous run's cache value is reset.
LOOPBACK_FLAG="OFF"
[[ "${LOOPBACK:-0}" != "0" ]] && LOOPBACK_FLAG="ON"

echo ">> Building rmcs_ecat core1+core0 (preset: $PRESET, loopback: $LOOPBACK_FLAG)"
cmake --preset "$PRESET" -S "$ECAT_DIR" \
    -DRMCS_ECAT_CORE1_LOOPBACK="$LOOPBACK_FLAG"
# Force a core1 relink so the embedded sec_core_img.c always matches the
# requested variant (the emit step only runs when core1 itself relinks).
touch "$ECAT_DIR/core1/src/main.cpp"
cmake --build "$ECAT_DIR/build" --target rmcs_ecat_core0

shopt -s nullglob
DFU_IMAGES=("$OUTPUT_DIR"/rmcs_ecat_bridge_*.dfu)
shopt -u nullglob
if [[ ${#DFU_IMAGES[@]} -eq 0 ]]; then
    echo "error: no rmcs_ecat_bridge_*.dfu found in $OUTPUT_DIR after the build." >&2
    exit 1
elif [[ ${#DFU_IMAGES[@]} -gt 1 ]]; then
    echo "error: multiple .dfu images in $OUTPUT_DIR; clean the build dir:" >&2
    printf '   %s\n' "${DFU_IMAGES[@]}" >&2
    exit 1
fi
DFU_IMAGE="${DFU_IMAGES[0]}"

# Read the VENDOR:PRODUCT the board baked into the DFU suffix, so the -d match
# always follows the actual board instead of a hard-coded pair.
DFU_SUFFIX="$(dfu-suffix -c "$DFU_IMAGE" 2>/dev/null)"
DFU_VID="$(printf '%s\n' "$DFU_SUFFIX" | sed -n 's/.*Vendor ID:[^0-9a-fA-Fx]*0x\([0-9a-fA-F]*\).*/\1/p')"
DFU_PID="$(printf '%s\n' "$DFU_SUFFIX" | sed -n 's/.*Product ID:[^0-9a-fA-Fx]*0x\([0-9a-fA-F]*\).*/\1/p')"
if [[ -z "$DFU_VID" || -z "$DFU_PID" ]]; then
    echo "error: could not read the USB id from $DFU_IMAGE's DFU suffix." >&2
    exit 1
fi
DFU_ID="$(printf '0x%s:0x%s' "${DFU_VID,,}" "${DFU_PID,,}") " # alt 0 = Internal Flash
DFU_ID="${DFU_ID// /}"

echo ">> ecat bridge image (core0 + embedded core1): $DFU_IMAGE"
echo ">> DFU devices currently visible (matching $DFU_ID):"
dfu-util -l 2>/dev/null | grep -i "${DFU_VID,,}:${DFU_PID,,}" || \
    echo "   (none in DFU mode yet; dfu-util will try to detach a running app)"

echo ">> Flashing needs root to access the DFU USB device; you may be asked for your password."
sudo dfu-util -d "$DFU_ID" -a 0 -D "$DFU_IMAGE"

echo ">> Done. The bootloader verifies the image and resets into the app."
echo "   (A 'lost device' / status-read error from dfu-util at the end is normal.)"
