#!/usr/bin/env bash
#
# Build and flash the HPM6E8Y core-swap image over USB DFU.
#
# This is the layout of firmware/rmcs_board/ecat/CORE_SWAP_MIGRATION.md:
# core0 runs USB + CAN + the librmcs protocol stack, core1 runs the EtherCAT
# stack, and the two transports arbitrate for one shared data plane. It is built
# from firmware/rmcs_board (the superbuild) with LIBRMCS_RELEASE_CORE1=ON, NOT
# from firmware/rmcs_board/ecat -- that directory still builds the older
# core0=SSC / core1=CAN bridge, which flash-ecat.sh handles.
#
# The three images are mutually exclusive; flashing any one overwrites the
# others:
#   ./flash-ecat.sh        EtherCAT bridge, old layout (core0 = SSC)
#   ./flash-ecat-swap.sh   core-swap layout (this script)
#   CORE1=0 ./flash-ecat-swap.sh
#                          plain single-core USB firmware (core1 not released),
#                          which is the fastest USB configuration -- see the
#                          selection note in firmware/rmcs_board/AGENTS.md
#
# Usage:
#   ./flash-ecat-swap.sh
#   BUILD_ONLY=1 ./flash-ecat-swap.sh
#   PRESET=debug ./flash-ecat-swap.sh
#   CORE1=0 ./flash-ecat-swap.sh      # single-core USB image
#   CAN_DIAG=1 ./flash-ecat-swap.sh   # add the UART0 CAN telemetry record
#                                     # (host/examples/can_stall_probe.cpp)
#   DIAG_USB=1 ./flash-ecat-swap.sh   # relay core1's log to the host over USB
#                                     # instead of the (unpopulated) console UART
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD_DIR="$SCRIPT_DIR/firmware/rmcs_board"
BOARD="${BOARD:-hpm6e8y}"
BUILD_DIR="${BUILD_DIR:-$BOARD_DIR/build_${BOARD}_swap}"
OUTPUT_DIR="$BUILD_DIR/app/output"
PRESET="${PRESET:-release}"
BUILD_ONLY="${BUILD_ONLY:-0}"

CORE1_FLAG="ON"
if [[ "${CORE1:-1}" == "0" ]]; then
    CORE1_FLAG="OFF"
fi
CAN_DIAG_FLAG="OFF"
if [[ "${CAN_DIAG:-0}" != "0" ]]; then
    CAN_DIAG_FLAG="ON"
fi
DIAG_USB_FLAG="OFF"
if [[ "${DIAG_USB:-0}" != "0" ]]; then
    DIAG_USB_FLAG="ON"
fi

if [[ -z "${GNURISCV_TOOLCHAIN_PATH:-}" ]]; then
    for candidate in \
        "$HOME/3rd_party/rv32imac_zicsr_zifencei_multilib_b_ext-linux" \
        "$HOME/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux"; do
        if [[ -x "$candidate/bin/riscv32-unknown-elf-gcc" ]]; then
            export GNURISCV_TOOLCHAIN_PATH="$candidate"
            break
        fi
    done
fi

: "${GNURISCV_TOOLCHAIN_PATH:?GNURISCV_TOOLCHAIN_PATH must point to the RISC-V toolchain root}"
if [[ ! -x "$GNURISCV_TOOLCHAIN_PATH/bin/riscv32-unknown-elf-gcc" ]]; then
    echo "error: riscv32-unknown-elf-gcc not found under $GNURISCV_TOOLCHAIN_PATH/bin" >&2
    exit 1
fi
export PATH="$GNURISCV_TOOLCHAIN_PATH/bin:$PATH"

command -v dfu-suffix >/dev/null 2>&1 || {
    echo "error: dfu-suffix not found (dfu-util >= 0.11 is required)" >&2
    exit 1
}

echo ">> Building core-swap image" \
    "(preset: $PRESET, BOARD=$BOARD, release_core1: $CORE1_FLAG," \
    "can_diag: $CAN_DIAG_FLAG, diag_over_usb: $DIAG_USB_FLAG)"

cmake --preset "$PRESET" -S "$BOARD_DIR" -B "$BUILD_DIR" \
    -DBOARD="$BOARD" \
    -DLIBRMCS_RELEASE_CORE1="$CORE1_FLAG" \
    -DLIBRMCS_CAN_DIAG="$CAN_DIAG_FLAG" \
    -DLIBRMCS_DIAG_OVER_USB="$DIAG_USB_FLAG"

# The superbuild orders core1 before the app, because the app compiles the
# sec_core_img.c the core1 project emits.
cmake --build "$BUILD_DIR" --target rmcs_board_app

DFU_IMAGE="$OUTPUT_DIR/rmcs_board_app_${BOARD}.dfu"
if [[ ! -f "$DFU_IMAGE" ]]; then
    echo "error: $DFU_IMAGE not found after the build" >&2
    exit 1
fi

# dfu-suffix validates the standard 16-byte suffix CRC. Validate the project
# ImageHash suffix immediately before it as well: magic + SHA-256(payload).
DFU_SUFFIX="$(dfu-suffix -c "$DFU_IMAGE" 2>&1)"
DFU_SIZE="$(stat -c '%s' "$DFU_IMAGE")"
readonly PROJECT_SUFFIX_SIZE=36
readonly DFU_SUFFIX_SIZE=16
if ((DFU_SIZE <= PROJECT_SUFFIX_SIZE + DFU_SUFFIX_SIZE)); then
    echo "error: DFU image is too small to contain both required suffixes" >&2
    exit 1
fi
PAYLOAD_SIZE=$((DFU_SIZE - PROJECT_SUFFIX_SIZE - DFU_SUFFIX_SIZE))
HASH_MAGIC="$(od -An -tx4 -j "$PAYLOAD_SIZE" -N4 "$DFU_IMAGE" | tr -d ' \n')"
EXPECTED_HASH="$(
    dd if="$DFU_IMAGE" bs=1 skip=$((PAYLOAD_SIZE + 4)) count=32 status=none \
        | od -An -tx1 -v | tr -d ' \n'
)"
ACTUAL_HASH="$(head -c "$PAYLOAD_SIZE" "$DFU_IMAGE" | sha256sum | awk '{print $1}')"
if [[ "$HASH_MAGIC" != "48415348" || "$EXPECTED_HASH" != "$ACTUAL_HASH" ]]; then
    echo "error: invalid ImageHash suffix in $DFU_IMAGE" >&2
    exit 1
fi

DFU_VID="$(printf '%s\n' "$DFU_SUFFIX" \
    | sed -n 's/.*Vendor ID:[^0-9a-fA-Fx]*0x\([0-9a-fA-F]*\).*/\1/p')"
DFU_PID="$(printf '%s\n' "$DFU_SUFFIX" \
    | sed -n 's/.*Product ID:[^0-9a-fA-Fx]*0x\([0-9a-fA-F]*\).*/\1/p')"
if [[ -z "$DFU_VID" || -z "$DFU_PID" ]]; then
    echo "error: could not read USB IDs from $DFU_IMAGE" >&2
    exit 1
fi
DFU_ID="$(printf '0x%s:0x%s' "${DFU_VID,,}" "${DFU_PID,,}")"

echo ">> Verified $DFU_IMAGE"
echo "   payload: $PAYLOAD_SIZE bytes"
echo "   SHA-256: $ACTUAL_HASH"
echo "   USB ID: $DFU_ID"

if [[ "$BUILD_ONLY" != "0" ]]; then
    echo ">> BUILD_ONLY requested; no device was written"
    exit 0
fi

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found (need >= 0.11)" >&2
    exit 1
}
command -v lsusb >/dev/null 2>&1 || {
    echo "error: lsusb not found" >&2
    exit 1
}

MATCHING_DEVICES="$(lsusb 2>/dev/null \
    | grep -Eic "ID ${DFU_VID}:${DFU_PID}( |$)" || true)"
if [[ "$MATCHING_DEVICES" != "1" ]]; then
    echo "error: expected exactly one $DFU_ID USB device, found $MATCHING_DEVICES" >&2
    echo "       connect USB0 and put the board in app runtime or DFU mode" >&2
    exit 1
fi

echo ">> Flashing application slot over USB DFU"
echo "   Keep power stable until dfu-util finishes the manifest phase."
sudo dfu-util -d "$DFU_ID" -a 0 -D "$DFU_IMAGE"

echo ">> DFU download completed; the bootloader will validate and cold-reset into the app"
