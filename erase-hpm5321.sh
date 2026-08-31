#!/usr/bin/env bash
#
# One-command flash erase for the HPM5321 board over J-Link/JTAG.
#
# Ozone can erase as well, but only by typing Target.EraseChip() into its Console:
# the manual lists "GUI Access: None" for that command (UM08025 section 7.9.18.1),
# and it always wipes the whole chip. This script is the one-liner equivalent, and
# it defaults to the reversible half of the job.
#
# Flash map -- 1 MB XPI0 NOR, from boards/hpm5321/hpm5321.yaml and
# boards/hpm5321/linker/app_flash_uf2.ld (UF2_BOOTLOADER_RESERVED_LENGTH 0x20000):
#
#   0x80000000 .. 0x8001FFFF   DFU bootloader
#   0x80020000 .. 0x800FFFFF   application + its SHA-256 metadata record
#
# Erasing only the upper region leaves the DFU bootloader in place, so the board
# still enumerates as 0xa11c:0xa902 and ./flash-dual.sh keeps working without a
# debugger. A full chip erase takes the bootloader with it: after that the only
# way back in is the debugger, via ./ozone-debug.sh hpm5321 (its project is
# CM_DOWNLOAD_RESET, so opening it reprograms the bootloader).
#
# Usage:
#   ./erase-hpm5321.sh            # erase the application only (default, reversible over DFU)
#   ./erase-hpm5321.sh --all      # full chip erase, bootloader included
#   ./erase-hpm5321.sh --dry-run  # print the J-Link script instead of running it
#
# Environment overrides:
#   SPEED=<kHz>     JTAG clock (default 4000, same as ozone/hpm5321.jdebug)
#   DEVICE=<name>   J-Link device name (default HPM5321xEGx -- the real die; the
#                   yaml's HPM5361 is the SDK build target, not the debugger name)
#   JLINK=<path>    JLinkExe to use (default: whatever is on PATH)
#
set -euo pipefail

BOOTLOADER_BASE=0x80000000
APP_BASE=0x80020000
FLASH_END=0x800FFFFF

DEVICE="${DEVICE:-HPM5321xEGx}"
SPEED="${SPEED:-4000}"
JLINK="${JLINK:-$(command -v JLinkExe || true)}"

MODE=app
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
    --all) MODE=all ;;
    --app) MODE=app ;;
    --dry-run) DRY_RUN=1 ;;
    -h | --help)
        sed -n '2,33p' "${BASH_SOURCE[0]}" | sed 's/^#\s\?//'
        exit 0
        ;;
    *)
        echo "error: unknown argument: $arg" >&2
        exit 2
        ;;
    esac
done

[[ -n "$JLINK" && -x "$JLINK" ]] || {
    echo "error: JLinkExe not found. Set JLINK=/path/to/JLinkExe." >&2
    exit 1
}

if [[ "$MODE" == all ]]; then
    ERASE_CMD="erase"
    VERIFY_ADDR="$BOOTLOADER_BASE"
    WHAT="ENTIRE CHIP ($BOOTLOADER_BASE .. $FLASH_END) -- DFU bootloader included"
else
    ERASE_CMD="erase $APP_BASE $FLASH_END"
    VERIFY_ADDR="$APP_BASE"
    WHAT="application region only ($APP_BASE .. $FLASH_END) -- bootloader kept"
fi

SCRIPT_FILE="$(mktemp -t hpm5321-erase.XXXXXX.jlink)"
trap 'rm -f "$SCRIPT_FILE"' EXIT
# EnableEraseAllFlashBanks is mandatory here, not optional. The program lives in
# the external XPI0 QSPI NOR, and J-Link restricts `erase` to internal flash banks
# unless this is set -- without it the erase reports "No Flash bank within given
# address range ..." and then still prints "Erasing done.", so it looks like it
# worked while nothing was touched.
cat >"$SCRIPT_FILE" <<JLINK_SCRIPT
connect
exec EnableEraseAllFlashBanks
$ERASE_CMD
mem32 $VERIFY_ADDR 0x4
exit
JLINK_SCRIPT

echo ">> device: $DEVICE   interface: JTAG @ ${SPEED} kHz"
echo ">> erasing: $WHAT"

if ((DRY_RUN)); then
    echo ">> dry run, J-Link script would be:"
    sed 's/^/     /' "$SCRIPT_FILE"
    exit 0
fi

if [[ "$MODE" == all ]]; then
    read -r -p ">> this removes the DFU bootloader too. Type 'erase' to confirm: " reply
    [[ "$reply" == erase ]] || {
        echo ">> aborted"
        exit 1
    }
fi

# -JTAGConf -1,-1 is auto-detect, matching ozone/hpm5321.jdebug's default chain
# position. Without it JLinkExe stops to ask interactively and the script stalls.
LOG_FILE="$(mktemp -t hpm5321-erase.XXXXXX.log)"
trap 'rm -f "$SCRIPT_FILE" "$LOG_FILE"' EXIT

"$JLINK" \
    -nogui 1 \
    -exitonerror 1 \
    -device "$DEVICE" \
    -if JTAG \
    -speed "$SPEED" \
    -JTAGConf -1,-1 \
    -CommanderScript "$SCRIPT_FILE" 2>&1 | tee "$LOG_FILE"

# J-Link prints "Erasing done." even when the address range matched no flash bank
# at all, so its exit status and its own summary line both lie. The only honest
# check is reading the first word back and seeing erased NOR (0xFFFFFFFF).
if grep -q "No Flash bank within given address range" "$LOG_FILE"; then
    echo >&2
    echo "error: J-Link found no flash bank covering the range -- nothing was erased." >&2
    echo "       The program lives in external XPI0 QSPI NOR; check that" >&2
    echo "       'exec EnableEraseAllFlashBanks' was accepted above." >&2
    exit 1
fi

if ! grep -qi "$(printf '%08X' "$VERIFY_ADDR").*FFFFFFFF" "$LOG_FILE"; then
    echo >&2
    echo "warning: read-back at $VERIFY_ADDR is not 0xFFFFFFFF -- the erase may not" >&2
    echo "         have taken. See the mem32 output above." >&2
    exit 1
fi

echo
echo ">> verified: $VERIFY_ADDR reads 0xFFFFFFFF"
