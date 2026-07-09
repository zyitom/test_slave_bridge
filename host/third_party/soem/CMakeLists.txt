# Vendored SOEM (Simple Open EtherCAT Master), GPLv2 + linking exception
# (see upstream/LICENSE -- the exception permits linking librmcs, under its own
# license, against SOEM). upstream/ is a verbatim copy of the SOEM 1.4.0 source
# tree (build artifacts removed); THIS file is librmcs build glue so the
# EtherCAT (SOEM) transport builds inline with the host SDK, with no system
# SOEM install. Re-sync: replace upstream/ with a newer SOEM release tree and
# re-verify the source globs and the nicdrv selection below. See README.md.

set(_soem "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

file(GLOB _soem_core "${_soem}/soem/*.c")
file(GLOB _soem_osal "${_soem}/osal/linux/*.c")

# oshw/linux is the hardware/OS shim. oshw.c is always in; nicdrv.c is the raw
# packet I/O and is the one file we swap for the AF_XDP zero-copy driver.
set(_soem_oshw "${_soem}/oshw/linux/oshw.c")

# AF_XDP nicdrv: busy-poll zero-copy path that removes the AF_PACKET
# skb/RX-interrupt/NAPI cost from every stop-and-wait cycle (see README.afxdp.md
# for the measured effect and the NIC/kernel prerequisites). Needs libxdp +
# libbpf; keep OFF on boxes without them -- the stock AF_PACKET nicdrv still
# builds and runs everywhere.
option(LIBRMCS_SOEM_AFXDP
    "Use the AF_XDP zero-copy SOEM nicdrv (needs libxdp/libbpf) instead of AF_PACKET" OFF)
if(LIBRMCS_SOEM_AFXDP)
    list(APPEND _soem_oshw "${CMAKE_CURRENT_SOURCE_DIR}/nicdrv_afxdp.c")
else()
    list(APPEND _soem_oshw "${_soem}/oshw/linux/nicdrv.c")
endif()

add_library(soem STATIC ${_soem_core} ${_soem_osal} ${_soem_oshw})
set_target_properties(soem PROPERTIES POSITION_INDEPENDENT_CODE ON)

# EC_VER1 (the global ec_slave[] API used by src/transport/soem/soem.cpp) is
# SOEM's default (ethercattype.h), so no define is needed. Expose the source
# header directories -- which, unlike an install, are NOT flattened -- to
# consumers as SYSTEM so <soem/ethercat.h> and its quoted siblings resolve and
# SOEM's headers do not trip the host's -Wall -Wextra -Wpedantic.
target_include_directories(soem SYSTEM PUBLIC
    "${_soem}"
    "${_soem}/soem"
    "${_soem}/osal"
    "${_soem}/osal/linux"
    "${_soem}/oshw/linux")

# SOEM's C uses POSIX/GNU symbols (nanosleep, clock_gettime, timeradd,
# IFF_PROMISC, PTHREAD_PRIO_INHERIT). The host builds with CMAKE_C_EXTENSIONS
# OFF (strict -std=c11) which hides them; SOEM's own build is gnu11. _GNU_SOURCE
# re-exposes them regardless of -std (the AF_XDP nicdrv self-defines the same).
target_compile_definitions(soem PRIVATE _GNU_SOURCE)

# Third-party: silence upstream warnings (SOEM builds -Wall -Wextra -Werror).
target_compile_options(soem PRIVATE -w)
target_link_libraries(soem PUBLIC pthread rt)

if(LIBRMCS_SOEM_AFXDP)
    find_library(XDP_LIBRARY xdp REQUIRED)
    find_library(BPF_LIBRARY bpf REQUIRED)
    target_link_libraries(soem PUBLIC ${XDP_LIBRARY} ${BPF_LIBRARY})
    message(STATUS "SOEM nicdrv: AF_XDP zero-copy (${XDP_LIBRARY} ${BPF_LIBRARY})")
else()
    message(STATUS "SOEM nicdrv: AF_PACKET (stock)")
endif()
