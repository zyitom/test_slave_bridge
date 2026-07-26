#!/usr/bin/env bash
#
# Flash the complete mc02 board over SWD with J-Link, and boot into the app.
#
# WHY THIS EXISTS. Programming mc02_app.elf with a debug probe -- Ozone, a gdb
# `load`, JLinkExe -- writes the image but nothing else, and the board then sits
# in DFU forever no matter how valid that image is. The bootloader refuses to
# leave DFU until flash also holds a metadata record marking the application as
# ready (firmware/mc02/bootloader/src/flash/metadata.hpp, validate_app_image()),
# and only the DFU download path ever writes that record. This script writes it
# too, so the SWD route ends in a running application like the DFU route does.
#
# It also appends the ImageHash suffix the bootloader verifies (SHA-256 over the
# image), which a plain `objcopy -O binary` does not produce -- another reason
# hand-flashed images are rejected.
#
# Programs, in order: bootloader (sector 0), metadata record (sector 1),
# application image (sectors 2+). Each loadfile erases only the sectors it
# touches, so the three writes do not disturb each other.
#
# Usage:
#   ./flash-mc02-swd.sh                  # build + flash everything, then run
#   PRESET=debug ./flash-mc02-swd.sh     # flash a debuggable (-O0) build
#   APP_ONLY=1 ./flash-mc02-swd.sh       # keep the resident bootloader
#
# Environment overrides:
#   PRESET=debug|release  build preset (default release, matching flash-mc02.sh)
#   APP_ONLY=1            skip the bootloader, program metadata + app only
#   SPEED=<kHz>           probe clock (default 4000)
#   JLINK_DEVICE=<name>   override the SEGGER device name
#
# For deployment over USB (no probe needed) use ./flash-mc02.sh instead.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/firmware/mc02/build"
OBJCOPY="${OBJCOPY:-/opt/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-objcopy}"

DEVICE="${JLINK_DEVICE:-STM32H723VGTx}"
SPEED="${SPEED:-4000}"
PRESET="${PRESET:-release}"

# Flash layout, mirroring firmware/mc02/bootloader/src/flash/layout.hpp.
BOOTLOADER_ADDRESS=0x08000000
METADATA_ADDRESS=0x08020000
APP_ADDRESS=0x08040000

command -v JLinkExe >/dev/null 2>&1 || {
    echo "error: JLinkExe not found. Install the SEGGER J-Link tools." >&2
    exit 1
}
[[ -x "$OBJCOPY" ]] || {
    echo "error: objcopy not found: $OBJCOPY (override with OBJCOPY=)" >&2
    exit 1
}

echo ">> Building mc02 (preset: $PRESET)"
cmake --preset "$PRESET" -S "$SCRIPT_DIR/firmware/mc02"
cmake --build "$BUILD_DIR" --target mc02_app mc02_bootloader

APP_ELF="$BUILD_DIR/app/mc02_app.elf"
APP_RAW="$BUILD_DIR/app/mc02_app.bin"
APP_IMAGE="$BUILD_DIR/app/mc02_app_image.bin"
METADATA_BIN="$BUILD_DIR/app/mc02_metadata.bin"
BOOTLOADER_ELF="$BUILD_DIR/bootloader/mc02_bootloader.elf"
BOOTLOADER_BIN="$BUILD_DIR/bootloader/mc02_bootloader.bin"

[[ -f "$APP_RAW" ]] || {
    echo "error: $APP_RAW not found after the build." >&2
    exit 1
}

# The bootloader validates the image WITH its ImageHash suffix and records that
# suffixed length, so the bytes programmed here must be the suffixed ones -- the
# same bytes dfu-util downloads out of mc02_app.dfu.
echo ">> Appending ImageHash suffix"
"$SCRIPT_DIR/.scripts/append_image_hash" -o "$APP_IMAGE" "$APP_RAW"

echo ">> Generating metadata record"
"$SCRIPT_DIR/.scripts/make_metadata_record" -o "$METADATA_BIN" "$APP_IMAGE"

JLINK_SCRIPT="$(mktemp -t mc02-flash-XXXXXX.jlink)"
trap 'rm -f "$JLINK_SCRIPT"' EXIT INT TERM

{
    echo "r"
    echo "h"
    if [[ -z "${APP_ONLY:-}" ]]; then
        "$OBJCOPY" -O binary "$BOOTLOADER_ELF" "$BOOTLOADER_BIN"
        echo "loadfile $BOOTLOADER_BIN, $BOOTLOADER_ADDRESS"
    fi
    echo "loadfile $METADATA_BIN, $METADATA_ADDRESS"
    echo "loadfile $APP_IMAGE, $APP_ADDRESS"
    echo "r"
    echo "g"
    echo "qc"
} >"$JLINK_SCRIPT"

echo ">> Programming over SWD (device $DEVICE @ ${SPEED}kHz)"
sed 's/^/   /' "$JLINK_SCRIPT"

JLinkExe -device "$DEVICE" -if SWD -speed "$SPEED" -autoconnect 1 \
    -NoGui 1 -ExitOnError 1 -CommanderScript "$JLINK_SCRIPT"

echo ">> Done. The bootloader validates the metadata record and jumps to the app."
