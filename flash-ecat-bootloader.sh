#!/usr/bin/env bash
#
# Build and flash the HPM6E8Y EtherCAT DFU bootloader through the HPM Boot ROM.
#
# The flash_xip binary starts at 0x80000400, not 0x80000000. Writing it at the
# XIP base shifts the FCFG table and makes the ROM fall back to ISP on boot.
# This script validates the image header before allowing any flash operation.
#
# Put the board in HPM6E00 Boot ROM USB mode (34b7:0006) before flashing.
# The application is updated separately with ./flash-ecat.sh.
#
# Usage:
#   ./flash-ecat-bootloader.sh
#   BUILD_ONLY=1 ./flash-ecat-bootloader.sh
#   HPM_MANUFACTURING_TOOL=/path/to/hpm_manufacturing_cmd ./flash-ecat-bootloader.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ECAT_DIR="$SCRIPT_DIR/firmware/rmcs_board/ecat"
BOARD="${BOARD:-hpm6e8y}"
BUILD_DIR="${BUILD_DIR:-$ECAT_DIR/build_${BOARD}}"
PRESET="${PRESET:-release}"
BUILD_ONLY="${BUILD_ONLY:-0}"

if [[ "$BOARD" != "hpm6e8y" ]]; then
    echo "error: this recovery script only has validated FCFG data for BOARD=hpm6e8y" >&2
    exit 1
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

echo ">> Building rmcs_ecat_bootloader (preset: $PRESET, BOARD=$BOARD)"
cmake --preset "$PRESET" -S "$ECAT_DIR" -B "$BUILD_DIR" \
    -DBOARD="$BOARD" \
    -DBOARD_SEARCH_PATH="$SCRIPT_DIR/firmware/rmcs_board/boards"
cmake --build "$BUILD_DIR" --target rmcs_ecat_bootloader

BL_BIN="$BUILD_DIR/rmcs_ecat_bootloader/output/rmcs_board_bootloader_${BOARD}.bin"
if [[ ! -f "$BL_BIN" ]]; then
    echo "error: $BL_BIN not found after the build" >&2
    exit 1
fi

# 0x80000400..0x8001efff is the bootloader image region. Metadata starts at
# 0x8001f000 and must never be overwritten by this raw image.
readonly BOOTLOADER_MAX_SIZE=$((0x8001f000 - 0x80000400))
BL_SIZE="$(stat -c '%s' "$BL_BIN")"
if ((BL_SIZE > BOOTLOADER_MAX_SIZE)); then
    echo "error: bootloader is $BL_SIZE bytes; maximum is $BOOTLOADER_MAX_SIZE bytes" >&2
    exit 1
fi

read -r FCFG_HEADER FCFG_OPT0 FCFG_OPT1 FCFG_RESERVED \
    < <(od -An -tx4 -N16 "$BL_BIN")
if [[ "$FCFG_HEADER" != "fcf90002" || "$FCFG_OPT0" != "00000007" \
      || "$FCFG_OPT1" != "00001000" || "$FCFG_RESERVED" != "00000000" ]]; then
    echo "error: unexpected HPM6E8Y FCFG at binary offset 0" >&2
    printf '       got: %s %s %s %s\n' \
        "$FCFG_HEADER" "$FCFG_OPT0" "$FCFG_OPT1" "$FCFG_RESERVED" >&2
    exit 1
fi

echo ">> Verified $BL_BIN"
echo "   size: $BL_SIZE bytes (limit: $BOOTLOADER_MAX_SIZE)"
echo "   FCFG: $FCFG_HEADER $FCFG_OPT0 $FCFG_OPT1 $FCFG_RESERVED"
echo "   flash address: 0x80000400"

if [[ "$BUILD_ONLY" != "0" ]]; then
    echo ">> BUILD_ONLY requested; no device was written"
    exit 0
fi

MANUFACTURING_TOOL_DIR="$HOME/3rd_party/hpm/HPMicro_Manufacturing_Tool_v0.6.0"
DEFAULT_MANUFACTURING_TOOL="$MANUFACTURING_TOOL_DIR/hpm_manufacturing_cmd"
MANUFACTURING_TOOL="${HPM_MANUFACTURING_TOOL:-$DEFAULT_MANUFACTURING_TOOL}"
if [[ "$MANUFACTURING_TOOL" == */* ]]; then
    if [[ ! -x "$MANUFACTURING_TOOL" ]]; then
        echo "error: HPM Manufacturing Tool not executable: $MANUFACTURING_TOOL" >&2
        exit 1
    fi
else
    MANUFACTURING_TOOL="$(command -v "$MANUFACTURING_TOOL" || true)"
    if [[ -z "$MANUFACTURING_TOOL" ]]; then
        echo "error: hpm_manufacturing_cmd not found; set HPM_MANUFACTURING_TOOL" >&2
        exit 1
    fi
fi

command -v lsusb >/dev/null 2>&1 || {
    echo "error: lsusb not found" >&2
    exit 1
}
ROM_DEVICES="$(lsusb 2>/dev/null | grep -ic '34b7:0006' || true)"
if [[ "$ROM_DEVICES" != "1" ]]; then
    echo "error: expected exactly one HPM6E00 Boot ROM device (34b7:0006), found $ROM_DEVICES" >&2
    echo "       force Boot ROM USB mode, reconnect USB0, and retry" >&2
    exit 1
fi

echo ">> Programming and checksum-verifying bootloader at 0x80000400"
echo "   Keep power stable until verify-checksum reports success."
sudo "$MANUFACTURING_TOOL" -t "${HPM_COMMAND_TIMEOUT_MS:-60000}" -u -f "HPM6E00,0" \
    -r "write-memory 0x0 0x200 [[0x$FCFG_HEADER,0x$FCFG_OPT0,0x$FCFG_OPT1,0x$FCFG_RESERVED]]" \
    -r "config-memory 0x10000 0x200" \
    -r "write-memory 0x10000 0x80000400 $BL_BIN" \
    -r "verify-checksum 0x10000 0x80000400 $BL_BIN"

echo ">> Bootloader write and checksum verification completed"
echo "   Release the Boot ROM strap and cold power-cycle the board."
echo "   Then flash the application with ./flash-ecat.sh."
