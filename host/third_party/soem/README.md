# Vendored SOEM + AF_XDP nicdrv

The EtherCAT (SOEM) host transport (`host/src/transport/soem/soem.cpp`) links
against SOEM. SOEM is vendored here so the transport builds inline with the host
SDK -- no system SOEM install, no separate "build SOEM, drop in the overlay,
reinstall" dance, and CI can build it.

## Layout

- `upstream/` -- verbatim SOEM 1.4.0 source tree (build/install/.git/test/doc
  removed). Do not edit; treat as read-only third party.
- `nicdrv_afxdp.c` -- our AF_XDP zero-copy replacement for
  `upstream/oshw/linux/nicdrv.c`. Same `ecx_*` signatures and buffer/index
  logic as the stock driver; only `setupnic`/`outframe`/`recvpkt`/`closenic`
  differ. Selected by a build flag, not by copying files.
- `CMakeLists.txt` -- build glue: builds the static `soem` target, exposes its
  headers, and picks the nicdrv.
- `README.afxdp.md` -- the AF_XDP driver's design, the measured latency effect,
  and the required NIC/kernel tuning.

## Build

Enable the transport from the host build (SOEM is built as part of it):

```bash
cmake --preset linux-debug -S host -DLIBRMCS_ENABLE_SOEM=ON
cmake --build host/build
```

For the low-latency zero-copy driver (needs `libxdp-dev` + `libbpf-dev`, a
supported NIC, and the tuning in `README.afxdp.md`):

```bash
cmake --preset linux-debug -S host -DLIBRMCS_ENABLE_SOEM=ON -DLIBRMCS_SOEM_AFXDP=ON
```

Default (`LIBRMCS_SOEM_AFXDP=OFF`) uses SOEM's stock AF_PACKET nicdrv, which
builds and runs anywhere -- useful for compile-checking on a box without libxdp.

## Latency note

Vendoring does not change latency -- it is a packaging/reproducibility choice.
The stream-latency floor is wire-locked (100BASE-TX serialization) plus the
firmware ARQ echo cycles; AF_XDP only removes the kernel socket-stack cost per
poll. See `README.afxdp.md` and `../../src/transport/igh/LATENCY_ROADMAP.md`.

## License

SOEM is GPLv2 with a linking exception (`upstream/LICENSE`): linking librmcs
against SOEM does not place librmcs under the GPL, but SOEM's own (and our
modified `nicdrv_afxdp.c`) source must remain available -- which it is, here.

## Re-syncing SOEM

Replace `upstream/` with a newer SOEM release source tree (drop
build/install/.git/test/doc), then re-check `CMakeLists.txt`: the `soem/*.c`,
`osal/linux/*.c`, `oshw/linux/*.c` globs and the include-dir list. A major bump
(e.g. SOEM 2.x, which reorganises the OSAL/OSHW layer and the context API) also
requires re-porting `nicdrv_afxdp.c` and `soem.cpp` to the new API.
