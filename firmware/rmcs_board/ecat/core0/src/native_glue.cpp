#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <librmcs/ecat/native_can.hpp>

#include "rmcs_pd.h"
#include "xcore_channel.hpp"

// Native CAN variant of the core0 process-data glue (RMCS_ECAT_NATIVE_CAN). It
// replaces the ARQ stream in pd_glue.cpp: the 48-byte PDO is treated as four
// 12-byte CAN mailboxes (one per bus, see native_can.hpp). Downlink mailboxes
// whose freshness counter changed are forwarded to core1 as fixed 16-byte
// records; uplink records from core1 are latched into the input mailboxes.
//
// core1 owns the CAN peripherals and runs the matching native poll loop; the
// protocol/session layer is bypassed in this variant.

namespace ecat = librmcs::firmware::ecat;
namespace native = librmcs::ecat;

namespace {

static_assert(
    RMCS_PD_CHUNK_SIZE == native::kNativePdSize,
    "SSC/ESI process data size must equal the native mailbox image (48 bytes)");

ecat::XcoreChannel* channel = nullptr;

// Last downlink freshness counter forwarded per bus, and the latched uplink
// mailbox image published back to the ESC. Core0-local, touched only from the
// SSC hook contexts.
std::uint8_t last_down_seq[native::kNativeBusCount] = {};
std::uint8_t up_image[native::kNativePdSize] = {};

void drain_uplink_records() {
    std::byte buffer[native::kNativeRecordSize * 8];
    for (;;) {
        const std::size_t got = channel->up.pop(buffer);
        if (got == 0)
            break;
        for (std::size_t offset = 0; offset + native::kNativeRecordSize <= got;
             offset += native::kNativeRecordSize) {
            const auto* record = reinterpret_cast<const std::uint8_t*>(buffer + offset);
            const std::uint8_t bus = record[native::kNativeRecordBusOffset];
            if (bus >= native::kNativeBusCount)
                continue;
            std::uint8_t* mailbox = up_image + bus * native::kNativeMailboxSize;
            // Copy meta + id + data straight from the record's mailbox tail,
            // then bump seq so the master observes a fresh frame.
            std::memcpy(
                mailbox + native::kNativeMetaOffset,
                record + native::kNativeRecordMailboxOffset + native::kNativeMetaOffset,
                native::kNativeMailboxSize - native::kNativeMetaOffset);
            mailbox[native::kNativeSeqOffset] =
                static_cast<std::uint8_t>(mailbox[native::kNativeSeqOffset] + 1);
            if (mailbox[native::kNativeSeqOffset] == 0)
                mailbox[native::kNativeSeqOffset] = 1;
        }
    }
}

} // namespace

extern "C" {

void rmcs_pd_init(void) { channel = &ecat::xcore_channel_init(); }

void rmcs_pd_reset(void) {
    std::memset(last_down_seq, 0, sizeof(last_down_seq));
    std::memset(up_image, 0, sizeof(up_image));
    channel->usb_active.store(0, std::memory_order::release);
    channel->link_epoch.fetch_add(1, std::memory_order::release);
}

void rmcs_pd_on_outputs(const uint8_t* pd) {
    // Forward every mailbox whose freshness counter advanced to core1 as a
    // 16-byte bus-tagged record.
    for (std::uint8_t bus = 0; bus < native::kNativeBusCount; ++bus) {
        const std::uint8_t* mailbox = pd + bus * native::kNativeMailboxSize;
        const std::uint8_t seq = mailbox[native::kNativeSeqOffset];
        if (seq == 0 || seq == last_down_seq[bus])
            continue;
        std::uint8_t record[native::kNativeRecordSize] = {};
        record[native::kNativeRecordBusOffset] = bus;
        std::memcpy(
            record + native::kNativeRecordMailboxOffset, mailbox, native::kNativeMailboxSize);
        if (channel->down.try_push(std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(record), native::kNativeRecordSize})) {
            last_down_seq[bus] = seq;
        }
    }
}

void rmcs_pd_build_inputs(uint8_t* pd) {
    drain_uplink_records();
    std::memcpy(pd, up_image, native::kNativePdSize);
}

bool rmcs_pd_uplink_pending(void) { return channel->up.readable() != 0; }

// --- USB shuttle (unused in this variant; kept for link compatibility) -------

size_t rmcs_pd_downlink_free(void) {
    return ecat::kXcoreDownRingSize - channel->down.readable();
}

size_t rmcs_pd_push_downlink(const uint8_t* data, size_t len) {
    if (len == 0)
        return 0;
    const bool ok = channel->down.try_push(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), len});
    return ok ? len : 0;
}

size_t rmcs_pd_pop_uplink(uint8_t* buffer, size_t capacity) {
    return channel->up.pop(std::span<std::byte>{reinterpret_cast<std::byte*>(buffer), capacity});
}

void rmcs_pd_set_usb_active(bool active) {
    // Native is an EtherCAT-only latency demonstrator with raw 16-byte records,
    // not a librmcs protocol stream. Keep the USB vendor pump inactive so it
    // cannot become a second consumer/producer of these SPSC rings.
    (void)active;
    channel->usb_active.store(0U, std::memory_order::release);
}

bool rmcs_pd_usb_active(void) { return false; }

} // extern "C"
