#pragma once

#include <cstddef>
#include <cstdint>

#include "xcore_ring.hpp"

namespace librmcs::firmware::ecat {

// Cross-core diagnostic log: core1 produces text bytes, core0 drains them to
// its console.
//
// WHY THIS EXISTS (do not "simplify" it away): core1 must never call printf.
// The SDK console keeps a single static UART pointer that only board_init()
// sets, and board_init_core1() deliberately does not (UART1 is this board's
// fieldbus data port, not a console). So on core1 g_console_uart stays NULL and
// uart_send_byte(NULL, ...) dereferences low addresses -- reading LSR at 0x34
// and writing THR at 0x20. On HPM6E80 those are NOT a per-core alias: local
// addresses 0x00000000..0x0003FFFF are core0's ILM (hpm_misc.h CORE0_ILM_
// LOCAL_BASE = 0, CORE1_ILM_LOCAL_BASE = 0x00040000), and core0's .vectors sits
// exactly at 0x00000000. A single printf on core1 therefore either spins
// forever waiting for a THR-empty bit that never comes, or corrupts core0's
// interrupt vector table. Both are undiagnosable from the symptom.
//
// The SSC stack and the SDK EtherCAT port layer printf on every error and
// first-boot path, so this is not a hypothetical. Routing those bytes here
// keeps the diagnostics AND removes ~25 KB of newlib float formatting plus
// libgcc soft-double from the core1 image.
//
// Lossy by design: a full ring drops bytes rather than stalling core1. A log
// message must never be able to block the EtherCAT data path.

inline constexpr std::size_t kXcoreDiagRingSize = 1024;

using XcoreDiagRing = XcoreRing<kXcoreDiagRingSize>;

// Producer side (core1). Returns the number of bytes accepted; a short return
// means the ring was full and the remainder was dropped. Safe from any context
// on core1: XcoreRing is SPSC and core1 is the only producer.
inline std::size_t xcore_diag_write(
    XcoreDiagRing& ring, const char* text, std::size_t size) noexcept {
    // try_push is all-or-nothing, so push byte-wise to degrade gracefully into
    // a partial message instead of dropping the whole line.
    std::size_t written = 0;
    while (written < size) {
        const auto byte = static_cast<std::byte>(text[written]);
        if (!ring.try_push({&byte, 1}))
            break;
        ++written;
    }
    return written;
}

// Consumer side (core0). Drains up to the buffer size and returns the count.
// Call from the main loop and hand the bytes to the console.
inline std::size_t xcore_diag_drain(
    XcoreDiagRing& ring, char* buffer, std::size_t capacity) noexcept {
    return ring.pop({reinterpret_cast<std::byte*>(buffer), capacity});
}

} // namespace librmcs::firmware::ecat
