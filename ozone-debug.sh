#!/usr/bin/env bash
#
# Open Ozone on a pre-configured project -- no New Project Wizard, no device
# picking. The projects live in ozone/*.jdebug and already carry the device,
# interface, speed, connect mode and program file.
#
# Usage:
#   ./ozone-debug.sh mc02             # STM32H723VGT6, SWD
#   ./ozone-debug.sh c_board          # STM32F407IGH6, SWD
#   ./ozone-debug.sh hpm6e8y          # HPM6E8Y core0 (EtherCAT bridge), JTAG
#   ./ozone-debug.sh hpm6e8y-core1    # HPM6E8Y core1 (fieldbus), JTAG
#   ./ozone-debug.sh --list
#
# Every project connects in "attach & halt" mode: it does NOT program the part.
# Flash with the flash-*.sh scripts first; the reason is in each .jdebug header.
#
# ch32_board is deliberately absent: it is debugged over WCH-Link, which Ozone
# does not speak. Use the VS Code "ch32 - Debug V3F boot core" configuration, or
# ./jlink-debug.sh for the command-line route on the J-Link boards.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/ozone"

OZONE="${OZONE:-}"
if [[ -z "$OZONE" ]]; then
    # Prefer whatever is on PATH, else the newest versioned install under /opt.
    if command -v Ozone >/dev/null 2>&1; then
        OZONE="$(command -v Ozone)"
    else
        for candidate in $(ls -d /opt/SEGGER/Ozone* 2>/dev/null | sort -r); do
            if [[ -x "$candidate/Ozone" ]]; then
                OZONE="$candidate/Ozone"
                break
            fi
        done
    fi
fi

list_projects() {
    echo "Available projects (ozone/):"
    for f in "$PROJECT_DIR"/*.jdebug; do
        [[ -e "$f" ]] || continue
        name="$(basename "$f" .jdebug)"
        device="$(sed -n 's/.*Project\.SetDevice *("\([^"]*\)").*/\1/p' "$f" | head -1)"
        iface="$(sed -n 's/.*Project\.SetTargetIF *("\([^"]*\)").*/\1/p' "$f" | head -1)"
        printf '  %-16s %-18s %s\n' "$name" "$device" "$iface"
    done
}

case "${1:-}" in
-h | --help)
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^#\s\?//'
    exit 0
    ;;
--list)
    list_projects
    exit 0
    ;;
"")
    echo "error: no project given" >&2
    echo >&2
    list_projects >&2
    exit 1
    ;;
esac

PROJECT="$PROJECT_DIR/$1.jdebug"
[[ -f "$PROJECT" ]] || {
    echo "error: no such project: $PROJECT" >&2
    echo >&2
    list_projects >&2
    exit 1
}

[[ -n "$OZONE" && -x "$OZONE" ]] || {
    echo "error: Ozone not found. Set OZONE=/path/to/Ozone." >&2
    exit 1
}

# Warn instead of failing: Ozone can still open with a stale or missing image,
# and sometimes that is what you want (e.g. inspecting a board you did not build
# for on this machine).
ELF_REL="$(sed -n 's/.*File\.Open *("\([^"]*\)").*/\1/p' "$PROJECT" | head -1)"
if [[ -n "$ELF_REL" && ! -f "$PROJECT_DIR/$ELF_REL" ]]; then
    echo "warning: program file not built yet: $PROJECT_DIR/$ELF_REL" >&2
fi

echo ">> $OZONE $PROJECT"
exec "$OZONE" "$PROJECT"
