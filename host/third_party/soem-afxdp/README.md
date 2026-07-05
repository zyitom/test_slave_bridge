# SOEM AF_XDP nicdrv (minimal-latency EtherCAT master I/O)

Drop-in replacement for SOEM v1.4.0 `oshw/linux/nicdrv.c`. It keeps every SOEM
function signature and buffer/index contract identical and swaps **only** the
packet I/O from a kernel `AF_PACKET` raw socket to an **AF_XDP** socket
(zero-copy + busy poll). All of SOEM's protocol code and the entire librmcs
transport (`host/src/transport/soem/soem.cpp`) stay unchanged.

## Why

The `AF_PACKET` path DMAs into an skb, waits for the NIC RX interrupt + NAPI
softirq, then copies to the socket queue -- tens of us per cycle, on every
cycle because the master loop is stop-and-wait. That is the ~46 us/cycle wall
behind the ~21 kHz you measured. AF_XDP zero-copy lets the NIC DMA straight
into our UMEM, and `SO_PREFER_BUSY_POLL` + a `recvfrom()` kick runs the driver
NAPI inline on the cycle thread, so the return frame is pulled without waiting
for an interrupt. Expect the per-cycle round trip to drop into the single-digit
us range (roughly 5-10x the cycle rate), which is the real lever the sysctl
tuning could not reach.

## Requirements (target machine with the i225/i226 igc NIC)

- Linux kernel >= 5.11 (`SO_PREFER_BUSY_POLL` / `SO_BUSY_POLL_BUDGET`),
  >= 5.14 for igc XDP; zero-copy on igc landed shortly after. The driver
  automatically falls back to `XDP_COPY` if ZC is unavailable (still faster
  than `AF_PACKET`, but confirm ZC for the lowest latency).
- `libxdp` + `libbpf` dev packages:
  `sudo apt install libxdp-dev libbpf-dev` (Ubuntu 22.04+/Debian 12+).
- Run as root or with `CAP_NET_RAW` + `CAP_BPF` (your `sudo` already covers it).

## Build: rebuild SOEM against this file

SOEM is a system/3rd-party library here (`find_package(soem)` /
`find_library(soem)`), so the swap happens in the SOEM build, not librmcs.

```bash
# 1. Back up and replace the Linux nicdrv in your SOEM source tree
cd <soem-src>            # e.g. ~/3rd_party/soem-1.4.0 (dev box) or the TL101 copy
cp oshw/linux/nicdrv.c oshw/linux/nicdrv.c.afpacket.bak
cp <librmcs>/host/third_party/soem-afxdp/nicdrv.c oshw/linux/nicdrv.c

# 2. Rebuild + reinstall SOEM (CMake build). SOEM's own CMakeLists does not link
#    libxdp/libbpf, so add them to the final link. Two options:
#    a) add  target_link_libraries(soem xdp bpf)  to SOEM's CMakeLists, or
#    b) leave SOEM as-is and let the librmcs exe pull them in (option below).
cmake --build build --target soem
cmake --install build      # or point librmcs at build/ output
```

Then build librmcs with the transport enabled and the AF_XDP libs linked:

```bash
cmake --preset linux-debug -S host -DLIBRMCS_ENABLE_SOEM=ON -DLIBRMCS_SOEM_AFXDP=ON
cmake --build host/build
```

`LIBRMCS_SOEM_AFXDP=ON` only adds `-lxdp -lbpf` to the final link (needed
because SOEM now references the `xsk_*` symbols). If you already linked them
inside SOEM (option 2a), the flag is harmless.

## NIC prep on the target (once per boot)

The EtherCAT RX must land on the queue the socket binds (default 0). Collapse
the NIC to a single combined queue so all traffic is on queue 0:

```bash
IFACE=enp2s0
sudo ethtool -L $IFACE combined 1        # all RX -> queue 0 (what we bind)
sudo ethtool -K $IFACE ntuple off        # no steering surprises
# keep your existing tuning: EEE off, C-states off, performance governor.
# You no longer need rx-usecs/busy_poll sysctls -- AF_XDP busy-poll replaces them.
```

Run the same test as before:

```bash
sudo taskset -c 6 ./host/build/examples/ecat_stream_latency enp2s0 10 7
```

You should see `[rmcs-afxdp] up on enp2s0 queue 0: ZERO-COPY, busy-poll on ...`
on stderr, a much higher `EtherCAT cycle rate`, and lower `rtt` percentiles.
(RTT stays ~4x the cycle period by the ARQ design in
`core/include/librmcs/ecat/pd_stream.hpp` -- watch the cycle rate as the
primary metric.)

## Tunables (environment, read once at setup)

| Var | Default | Meaning |
|-----|---------|---------|
| `RMCS_XDP_QUEUE` | 0 | RX/TX queue id to bind |
| `RMCS_XDP_COPY` | 0 | `1` forces `XDP_COPY` (skip the zero-copy attempt) |
| `RMCS_XDP_NO_BUSYPOLL` | 0 | `1` relies on softirq instead of inline NAPI |
| `RMCS_XDP_BUSYPOLL_US` | 20 | `SO_BUSY_POLL` value (us) |
| `RMCS_XDP_BUSYPOLL_BUDGET` | 8 | `SO_BUSY_POLL_BUDGET` (packets/poll) |

## Verify zero-copy actually engaged

- The stderr banner prints `ZERO-COPY` vs `COPY`.
- `sudo ethtool -S enp2s0 | grep -i xdp` shows `xdp_zc`/`rx_xdp` counters ticking.
- If it says `COPY`, ZC failed to bind: check kernel/driver version, and that
  the queue id exists (`ethtool -l enp2s0`).

## Rollback

Restore `oshw/linux/nicdrv.c.afpacket.bak` over `nicdrv.c`, rebuild SOEM, and
build librmcs without `-DLIBRMCS_SOEM_AFXDP=ON`. Nothing in librmcs proper
changed, so the AF_PACKET path returns exactly as before.
