#pragma once

#include <cstddef>
#include <cstdint>

// Native CAN mailbox transport over EtherCAT process data, selected by the
// RMCS_ECAT_NATIVE_CAN firmware variant. Instead of carrying the ARQ byte
// stream (see pd_stream.hpp), the fixed 48-byte PDO is partitioned into one
// 12-byte mailbox per CAN bus.
//
// Semantics are latest-wins: the producer bumps `seq` on every new frame it
// stages, and the consumer forwards a mailbox exactly once per observed `seq`
// change. Because the EtherCAT poll rate (~38 kHz here) vastly exceeds the
// per-bus frame rate (control is ~1 kHz), a mailbox is read many times before
// it can be overwritten, so no acknowledge/retransmit (ARQ) layer is needed --
// that is precisely why the native path removes the stream's per-message ack
// round trips.
//
// Mailbox wire layout (little-endian; both the host x86 and the RV32 firmware
// are little-endian, so a direct field read is safe):
//   offset 0: u8  seq   -- freshness counter; 1..255 wrapping (0 = no frame)
//   offset 1: u8  meta  -- bit7 = CAN-FD frame; bits0..3 = data length (0..8)
//   offset 2: u16 id    -- 11-bit standard CAN id (little-endian)
//   offset 4: u8  data[8]
namespace librmcs::ecat {

inline constexpr std::size_t kNativeBusCount = 4;
inline constexpr std::size_t kNativeMailboxSize = 12;
inline constexpr std::size_t kNativePdSize = kNativeBusCount * kNativeMailboxSize; // 48
inline constexpr std::size_t kNativeMaxDataSize = 8;

inline constexpr std::size_t kNativeSeqOffset = 0;
inline constexpr std::size_t kNativeMetaOffset = 1;
inline constexpr std::size_t kNativeIdOffset = 2;
inline constexpr std::size_t kNativeDataOffset = 4;

inline constexpr std::uint8_t kNativeMetaFdBit = 0x80U;
inline constexpr std::uint8_t kNativeMetaLenMask = 0x0FU;

constexpr std::uint8_t native_meta(bool is_fdcan, std::uint8_t length) {
    return static_cast<std::uint8_t>(
        (is_fdcan ? kNativeMetaFdBit : 0U) | (length & kNativeMetaLenMask));
}
constexpr bool native_meta_is_fdcan(std::uint8_t meta) { return (meta & kNativeMetaFdBit) != 0U; }
constexpr std::uint8_t native_meta_length(std::uint8_t meta) {
    return static_cast<std::uint8_t>(meta & kNativeMetaLenMask);
}

// Cross-core record (core0 <-> core1) for one bus/slot-tagged frame. Fixed 16
// bytes so the byte ring can be framed by a simple modulo: byte 0 is the bus
// index, byte 1 is the slot within that bus, bytes 2..3 carry the low 16 bits
// of link_epoch in the hybrid variant, and bytes 4..15 mirror the mailbox
// layout above (seq is unused across the cores). The native demonstrator leaves
// the epoch bytes unused and retains its original semantics.
inline constexpr std::size_t kNativeRecordSize = 16;
inline constexpr std::size_t kNativeRecordBusOffset = 0;
inline constexpr std::size_t kNativeRecordSlotOffset = 1;
inline constexpr std::size_t kNativeRecordEpochOffset = 2;
inline constexpr std::size_t kNativeRecordMailboxOffset = 4;

constexpr std::uint16_t native_record_epoch(const std::uint8_t* record) noexcept {
    return static_cast<std::uint16_t>(record[kNativeRecordEpochOffset])
         | static_cast<std::uint16_t>(record[kNativeRecordEpochOffset + 1]) << 8;
}

constexpr void native_record_set_epoch(std::uint8_t* record, std::uint16_t epoch) noexcept {
    record[kNativeRecordEpochOffset] = static_cast<std::uint8_t>(epoch);
    record[kNativeRecordEpochOffset + 1] = static_cast<std::uint8_t>(epoch >> 8);
}

static_assert(kNativeRecordEpochOffset + sizeof(std::uint16_t) == kNativeRecordMailboxOffset);

} // namespace librmcs::ecat
