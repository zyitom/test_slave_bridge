#pragma once

#include <cstddef>

#include <librmcs/ecat/native_can.hpp>

// Hybrid fixed-PDO layout, selected by the RMCS_ECAT_HYBRID_PD firmware
// variant. Each PDO direction is 352 bytes split into two regions:
//
//   offset   0..335  FIXED REGION  -- seven 12-byte cyclic CAN slots per bus,
//                                     28 slots total. Each slot has its own seq
//                                     gate, so one PacketBuilder batch can carry
//                                     a complete 4 x 7 motor control tick.
//   offset 336..351  STREAM REGION -- one 16-byte pd_stream ARQ chunk (4-byte
//                                     header + 12-byte payload). The full
//                                     librmcs protocol/session remains available
//                                     for configuration, UART, overflow and CAN
//                                     frames unsupported by the fixed format via
//                                     a separate reliable-stream transaction.
//
// This header is shared by the firmware (core0 glue) and the host raw-ecrt
// tools. It intentionally does NOT depend on pd_stream.hpp: the stream chunk
// size is fixed here at 16 bytes so a host SDK can address the fixed region
// independently of the templated ARQ endpoint. hybrid_glue.cpp statically
// checks this constant against kHybridPdChunkSize.
namespace librmcs::ecat {

inline constexpr std::size_t kHybridSlotsPerBus = 7;
inline constexpr std::size_t kHybridMailboxCount = kNativeBusCount * kHybridSlotsPerBus;
inline constexpr std::size_t kHybridMailboxRegionOffset = 0;
inline constexpr std::size_t kHybridMailboxRegionSize =
    kHybridMailboxCount * kNativeMailboxSize; // 336
inline constexpr std::size_t kHybridStreamRegionOffset = kHybridMailboxRegionSize; // 336
inline constexpr std::size_t kHybridStreamChunkSize = 16;
inline constexpr std::size_t kHybridPdSize =
    kHybridMailboxRegionSize + kHybridStreamChunkSize; // 352

constexpr std::size_t hybrid_mailbox_index(std::size_t bus, std::size_t slot) {
    return bus * kHybridSlotsPerBus + slot;
}

constexpr std::size_t hybrid_mailbox_offset(std::size_t bus, std::size_t slot) {
    return hybrid_mailbox_index(bus, slot) * kNativeMailboxSize;
}

static_assert(kHybridMailboxCount == 28, "hybrid layout must carry 4 x 7 CAN slots");
static_assert(kHybridMailboxRegionSize == 336, "mailbox region must be 28 x 12-byte slots");
static_assert(kHybridStreamChunkSize == 16, "stream chunk is a 4-byte header + 12-byte payload");
static_assert(kHybridStreamRegionOffset == 336, "stream region follows the mailbox region");
static_assert(kHybridPdSize == 352, "hybrid PDO is 352 bytes per direction");

} // namespace librmcs::ecat
