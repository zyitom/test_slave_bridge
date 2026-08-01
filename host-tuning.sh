#!/usr/bin/env bash
#
# Host-side latency tuning for the librmcs transports (USB bulk and EtherCAT/IgH).
#
# WHY THIS EXISTS: every setting below is runtime-only and reverts on reboot, and
# two of them have a mandatory ORDER that is easy to get wrong (the NIC must be
# tuned while the EtherCAT master does NOT hold it). Doing this by hand from a
# doc reliably produces a half-applied machine, and a half-applied machine
# produces latency numbers that look like transport problems.
#
# WHAT IS MEASURED AND WHAT IS NOT -- read before believing any of it
# (full data in firmware/rmcs_board/ecat/DESIGN.md sections 3.5 / 3.6):
#
#   PM QoS / C-states   MEASURED, LARGE. USB p99.9 188.2 -> 158.9 us,
#                       max 216.8 -> 160.7 us. USB blocks in libusb_handle_events
#                       so its core sleeps; EtherCAT busy-polls and never does,
#                       which is why only USB cares. NOT applied by this script:
#                       it needs a process to hold the fd open (see --pmqos).
#   RT throttling off   Assumed necessary, inherited from earlier work (a ~50 ms
#                       max latency was traced to it). NOT re-measured here.
#   governor            MEASURED, NO EFFECT (p50/p99/max all within noise). The
#                       SCHED_FIFO thread already holds the frequency up. Applied
#                       anyway because it is free and removes a variable.
#   NIC rx-usecs=0      ALMOST CERTAINLY A NO-OP HERE. Observed: set it to 0,
#                       start the master, stop the master, read it back -- it is
#                       3 again. ec_igc re-initializes the hardware when it takes
#                       the NIC, so the ethtool value never reaches the EtherCAT
#                       data path. Kept only because a future switch to
#                       ec_generic (which does go through the kernel stack) would
#                       make coalescing matter for real. Do NOT treat "rx-usecs
#                       is 0" as evidence the NIC has been tuned.
#   NIC EEE off         Already off on this machine; the script only verifies.
#   xHCI IRQ thread     MEASURED, NO EFFECT. Raising irq/N-xhci_hcd from the RT
#     RT priority       kernel default FIFO 50 to FIFO 90 changed nothing across
#                       3 runs: it sits on a different core from the libusb event
#                       thread, so the two never compete. Left at the default.
#   USB root hub        Set to power/control=on. NOT measured as a delta -- the
#     power/control     hub's runtime_status was already 'active', so it was never
#                       suspending. Applied to close off a class of surprise.
#   irqbalance          Must stay OFF (it would migrate the xHCI IRQ at runtime).
#                       Already inactive here; the script only verifies.
#
# The full write-up, including why USB and EtherCAT react differently to host
# tuning at all, is in HOST_TUNING.md at the repository root.
#
# EXPLICITLY DISPROVEN -- do NOT add these back:
#   Pinning the xHCI IRQ to the same core as the libusb event thread:
#   p50 124.8 -> 146.2 us, p90 126 -> 157 us. The ISR and the event thread
#   serialize on one core; that costs far more than the cross-core wakeup saves.
#   Leave the xHCI IRQ and the event thread on DIFFERENT cores.
#
# Usage:
#   sudo ./host-tuning.sh              apply everything except PM QoS, then report
#   sudo ./host-tuning.sh --check      report only, change nothing
#   sudo ./host-tuning.sh --pmqos      hold C-states off in the foreground
#                                      (Ctrl-C to release; run in a second
#                                      terminal for the duration of a measurement)
#
set -euo pipefail

ETHERCAT_IF="${ETHERCAT_IF:-enp2s0}"
CHECK_ONLY=0
PMQOS_ONLY=0
case "${1:-}" in
    --check) CHECK_ONLY=1 ;;
    --pmqos) PMQOS_ONLY=1 ;;
    "") ;;
    *) echo "usage: $0 [--check|--pmqos]" >&2; exit 1 ;;
esac

if [[ "$PMQOS_ONLY" == "0" && "$CHECK_ONLY" == "0" && "$EUID" -ne 0 ]]; then
    echo "error: must run as root to apply (use --check to only report)" >&2
    exit 1
fi

ok()   { printf '  \033[32m[ok]\033[0m    %s\n' "$1"; }
warn() { printf '  \033[33m[warn]\033[0m  %s\n' "$1"; }
info() { printf '  [info]  %s\n' "$1"; }

# --- PM QoS: hold deep C-states off for as long as this process lives --------
# Kept in this script rather than a systemd unit on purpose: it costs real power
# and it is only wanted while measuring, so making it a boot-time service would
# silently tax the machine forever.
if [[ "$PMQOS_ONLY" == "1" ]]; then
    if [[ "$EUID" -ne 0 ]]; then
        echo "error: --pmqos must run as root" >&2
        exit 1
    fi
    echo ">> Holding /dev/cpu_dma_latency at 0 (deep C-states disabled)."
    echo "   Leave this running for the duration of the measurement; Ctrl-C releases."
    exec python3 -c '
import os, struct, signal
fd = os.open("/dev/cpu_dma_latency", os.O_WRONLY)
os.write(fd, struct.pack("i", 0))
signal.pause()
'
fi

echo "== Kernel boot parameters (cannot be changed at runtime) =="
CMDLINE="$(cat /proc/cmdline)"
for want in isolcpus nohz_full rcu_nocbs; do
    if grep -q "$want=" <<<"$CMDLINE"; then
        ok "$want present: $(grep -o "${want}=[^ ]*" <<<"$CMDLINE")"
    else
        warn "$want missing -- add it to GRUB_CMDLINE_LINUX and reboot"
    fi
done
if uname -r | grep -qi "realtime\|rt"; then
    ok "PREEMPT_RT kernel: $(uname -r)"
else
    warn "not an RT kernel ($(uname -r)); tail latency will be worse"
fi

echo "== RT scheduler =="
RT_RUNTIME="$(cat /proc/sys/kernel/sched_rt_runtime_us)"
if [[ "$RT_RUNTIME" == "-1" ]]; then
    ok "sched_rt_runtime_us = -1 (throttling off)"
elif [[ "$CHECK_ONLY" == "1" ]]; then
    warn "sched_rt_runtime_us = $RT_RUNTIME (throttling ON -- run without --check)"
else
    echo -1 > /proc/sys/kernel/sched_rt_runtime_us
    ok "sched_rt_runtime_us: $RT_RUNTIME -> -1"
fi

echo "== CPU governor (measured: no effect; applied to remove a variable) =="
GOV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
if [[ "$GOV" == "n/a" ]]; then
    info "no cpufreq sysfs; skipping"
elif [[ "$GOV" == "performance" ]]; then
    ok "governor already performance"
elif [[ "$CHECK_ONLY" == "1" ]]; then
    warn "governor = $GOV (run without --check to set performance)"
else
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$g" 2>/dev/null || true
    done
    ok "governor: $GOV -> performance"
fi

echo "== EtherCAT NIC ($ETHERCAT_IF) =="
# ORDER MATTERS: while the master is running, ec_igc owns the NIC and it is not
# a netdev any more -- ethtool fails with "no device matches name". Stop, tune,
# start.
MASTER_WAS_RUNNING=0
if systemctl is-active --quiet ethercat 2>/dev/null; then
    MASTER_WAS_RUNNING=1
fi

if ! ip link show "$ETHERCAT_IF" >/dev/null 2>&1; then
    if [[ "$CHECK_ONLY" == "1" ]]; then
        warn "$ETHERCAT_IF not a netdev (EtherCAT master holds it); stop it to inspect"
    else
        info "stopping ethercat master to release $ETHERCAT_IF"
        systemctl stop ethercat
        sleep 3
    fi
fi

if ip link show "$ETHERCAT_IF" >/dev/null 2>&1; then
    RXU="$(ethtool -c "$ETHERCAT_IF" 2>/dev/null | sed -n 's/^rx-usecs:[[:space:]]*//p')"
    if [[ "$RXU" == "0" ]]; then
        ok "rx-usecs already 0"
    elif [[ "$CHECK_ONLY" == "1" ]]; then
        warn "rx-usecs = $RXU (run without --check to set 0)"
    else
        # igc runs Rx/Tx as a queue pair: setting tx-usecs is rejected outright,
        # only rx-usecs may be written. Do not "fix" this by adding tx-usecs.
        if ethtool -C "$ETHERCAT_IF" rx-usecs 0 >/dev/null 2>&1; then
            ok "rx-usecs: $RXU -> 0"
        else
            warn "driver rejected rx-usecs 0 (leaving $RXU)"
        fi
    fi

    if ethtool --show-eee "$ETHERCAT_IF" 2>/dev/null | grep -q "EEE status: disabled"; then
        ok "EEE disabled"
    elif [[ "$CHECK_ONLY" == "1" ]]; then
        warn "EEE not disabled"
    else
        ethtool --set-eee "$ETHERCAT_IF" eee off >/dev/null 2>&1 \
            && ok "EEE -> off" || warn "could not disable EEE"
    fi
fi

if [[ "$MASTER_WAS_RUNNING" == "1" && "$CHECK_ONLY" == "0" ]]; then
    systemctl start ethercat
    sleep 5
    ok "ethercat master restarted"
fi

echo "== USB device (a11c:a904) =="
USB_DEV=""
for d in /sys/bus/usb/devices/*/; do
    [[ "$(cat "$d/idVendor" 2>/dev/null)" == "a11c" ]] || continue
    [[ "$(cat "$d/idProduct" 2>/dev/null)" == "a904" ]] || continue
    USB_DEV="$d"
done
if [[ -z "$USB_DEV" ]]; then
    warn "board not enumerated"
else
    PCTL="$(cat "$USB_DEV/power/control" 2>/dev/null || echo n/a)"
    if [[ "$PCTL" == "on" ]]; then
        ok "power/control = on (autosuspend disabled)"
    elif [[ "$CHECK_ONLY" == "1" ]]; then
        warn "power/control = $PCTL"
    else
        echo on > "$USB_DEV/power/control" && ok "power/control: $PCTL -> on"
    fi
fi

# The root hub defaults to power/control=auto. It was never observed suspending
# (a hub cannot while a child is active), so this is hardening rather than a fix.
for hub in /sys/bus/usb/devices/usb*/power/control; do
    [[ -w "$hub" ]] || continue
    if [[ "$(cat "$hub")" != "on" && "$CHECK_ONLY" == "0" ]]; then
        echo on > "$hub" 2>/dev/null || true
    fi
done
if [[ "$CHECK_ONLY" == "0" ]]; then
    ok "root hubs power/control -> on"
fi

echo "== irqbalance (must stay off: it migrates the xHCI IRQ at runtime) =="
if systemctl is-active --quiet irqbalance 2>/dev/null; then
    warn "irqbalance is RUNNING -- stop and disable it"
else
    ok "irqbalance not running"
fi

echo "== C-states =="
if command -v lsof >/dev/null 2>&1 && lsof /dev/cpu_dma_latency >/dev/null 2>&1; then
    ok "/dev/cpu_dma_latency held (deep C-states disabled)"
else
    warn "deep C-states ENABLED -- USB tail latency will be ~30-60 us worse."
    warn "Run '$0 --pmqos' in another terminal while measuring USB."
fi

echo
echo ">> Done. Nothing here survives a reboot; re-run after every boot."
