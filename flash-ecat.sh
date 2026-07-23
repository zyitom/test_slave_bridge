#!/usr/bin/env bash
#
# Build and flash the HPM6E8Y EtherCAT bridge application over USB DFU.
#
# The DFU image contains core0 plus the embedded core1 image. The bootloader
# writes only the application slot at 0x80020000 and commits its ready metadata
# after validating the complete image.
#
# Usage:
#   ./flash-ecat.sh
#   BUILD_ONLY=1 ./flash-ecat.sh
#   PRESET=debug ./flash-ecat.sh
#   LOOPBACK=1 ./flash-ecat.sh
#   NATIVE=1 ./flash-ecat.sh    # native CAN mailbox variant (RMCS_ECAT_NATIVE_CAN)
#   HYBRID=1 ./flash-ecat.sh    # hybrid fixed-PDO variant (RMCS_ECAT_HYBRID_PD)
#   (NATIVE and HYBRID are mutually exclusive)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ECAT_DIR="$SCRIPT_DIR/firmware/rmcs_board/ecat"
BOARD="${BOARD:-hpm6e8y}"
BUILD_DIR="${BUILD_DIR:-$ECAT_DIR/build_${BOARD}}"
OUTPUT_DIR="$BUILD_DIR/rmcs_ecat_core0/output"
PRESET="${PRESET:-release}"
BUILD_ONLY="${BUILD_ONLY:-0}"

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

LOOPBACK_FLAG="OFF"
if [[ "${LOOPBACK:-0}" != "0" ]]; then
    LOOPBACK_FLAG="ON"
fi
NATIVE_FLAG="OFF"
if [[ "${NATIVE:-0}" != "0" ]]; then
    NATIVE_FLAG="ON"
fi
HYBRID_FLAG="OFF"
if [[ "${HYBRID:-0}" != "0" ]]; then
    HYBRID_FLAG="ON"
fi
if [[ "$NATIVE_FLAG" == "ON" && "$HYBRID_FLAG" == "ON" ]]; then
    echo "error: NATIVE and HYBRID are mutually exclusive; enable at most one" >&2
    exit 1
fi
if [[ "$LOOPBACK_FLAG" == "ON" && ( "$NATIVE_FLAG" == "ON" || "$HYBRID_FLAG" == "ON" ) ]]; then
    echo "error: LOOPBACK cannot be combined with NATIVE or HYBRID" >&2
    exit 1
fi

echo ">> Building rmcs_ecat core1+core0" \
    "(preset: $PRESET, BOARD=$BOARD, loopback: $LOOPBACK_FLAG," \
    "native: $NATIVE_FLAG, hybrid: $HYBRID_FLAG)"
cmake --preset "$PRESET" -S "$ECAT_DIR" -B "$BUILD_DIR" \
    -DBOARD="$BOARD" \
    -DBOARD_SEARCH_PATH="$SCRIPT_DIR/firmware/rmcs_board/boards" \
    -DRMCS_ECAT_CORE1_LOOPBACK="$LOOPBACK_FLAG" \
    -DRMCS_ECAT_NATIVE_CAN="$NATIVE_FLAG" \
    -DRMCS_ECAT_HYBRID_PD="$HYBRID_FLAG"
# Force a core1 relink so sec_core_img.c always matches the selected variant
# even when an existing nested build cache is reused. Touch the native/stream/
# hybrid glue and CAN driver too, since the variant changes their compilation.
touch "$ECAT_DIR/core1/src/main.cpp" \
    "$ECAT_DIR/core0/src/native_glue.cpp" "$ECAT_DIR/core0/src/pd_glue.cpp" \
    "$ECAT_DIR/core0/src/hybrid_glue.cpp" "$ECAT_DIR/core1/src/hybrid_link.cpp" \
    "$SCRIPT_DIR/firmware/rmcs_board/app/src/can/can.cpp"
cmake --build "$BUILD_DIR" --target rmcs_ecat_core0

DFU_IMAGE="$OUTPUT_DIR/rmcs_ecat_bridge_${BOARD}.dfu"
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
