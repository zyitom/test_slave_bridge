#!/usr/bin/env bash
#
# Flash the rmcs_ecat_bridge (hpm6e80ivm1) DFU BOOTLOADER via OpenOCD (one-time).
#
# Dual-core note: the HPM6E80 needs NO special dual-core flashing. Only core0's
# image lives in flash; the core1 program is embedded inside it as a C array
# (ecat/core0/src/sec_core_img.c) and core0 copies it into core1's ILM at boot
# (multicore_release_cpu). So the flash layout is exactly the single-core
# boards': bootloader @ 0x80000000 (this script), app @ +0x20000 (flash-ecat.sh).
#
# This is only needed for a blank chip or to recover/update the bootloader.
# After that, flash the application with ./flash-ecat.sh over USB DFU.
#
# Probe: the hpm6e00evk-design hardware has the FT2232 debugger on board --
# plug the USB "DEBUG" port in and no external J-Link is needed (PROBE=ft2232,
# the default). Use PROBE=jlink with an external J-Link on the JTAG header.
#
# Usage:
#   ./flash-ecat-bootloader.sh                 # builds the debug preset, then flashes
#   PROBE=jlink ./flash-ecat-bootloader.sh     # external J-Link instead of on-board FT2232
#   OPENOCD=/path/to/openocd ./flash-ecat-bootloader.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ECAT_DIR="$SCRIPT_DIR/firmware/rmcs_board/ecat"
BL_BIN="$ECAT_DIR/build/rmcs_ecat_bootloader/output/rmcs_board_bootloader_hpm6e80ivm1.bin"

HPM_SDK_BASE="$SCRIPT_DIR/firmware/rmcs_board/bsp/hpm_sdk"
OPENOCD_DIR="$HPM_SDK_BASE/boards/openocd"
OPENOCD_BIN="${OPENOCD:-openocd}"
PROBE="${PROBE:-ft2232}"

command -v "$OPENOCD_BIN" >/dev/null 2>&1 || {
    echo "error: openocd not found; set OPENOCD=/path/to/openocd" >&2
    echo "       (a build lives in ~/3rd_party/hpm/openocd-linux-x86_64)" >&2
    exit 1
}

PRESET="${PRESET:-release}"
# Default to the known local toolchain install when the env var is not set.
DEFAULT_TOOLCHAIN="$HOME/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux"
if [[ -z "${GNURISCV_TOOLCHAIN_PATH:-}" && -d "$DEFAULT_TOOLCHAIN" ]]; then
    export GNURISCV_TOOLCHAIN_PATH="$DEFAULT_TOOLCHAIN"
fi
: "${GNURISCV_TOOLCHAIN_PATH:?GNURISCV_TOOLCHAIN_PATH must point to the RISC-V toolchain root}"
echo ">> Building rmcs_ecat_bootloader (preset: $PRESET, BOARD=hpm6e80ivm1)"
cmake --preset "$PRESET" -S "$ECAT_DIR"
cmake --build "$ECAT_DIR/build" --target rmcs_ecat_bootloader

if [[ ! -f "$BL_BIN" ]]; then
    echo "error: $BL_BIN not found after the build." >&2
    exit 1
fi

echo ">> Flashing DFU bootloader @ 0x80000000 via $PROBE: $BL_BIN"
# BOARD hpm6e00evk supplies the xpi0 NOR flash bank; the hardware is that design.
"$OPENOCD_BIN" -s "$OPENOCD_DIR" \
    -c "set HPM_SDK_BASE $HPM_SDK_BASE; set BOARD hpm6e00evk; set PROBE $PROBE;" \
    -f hpm6e00_all_in_one.cfg \
    -c "program $BL_BIN 0x80000000 verify reset exit"

echo ">> Done. Now flash the bridge app over USB DFU:  ./flash-ecat.sh"
