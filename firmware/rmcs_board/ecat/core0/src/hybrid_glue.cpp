#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <librmcs/ecat/hybrid_pd.hpp>
#include <librmcs/ecat/native_can.hpp>
#include <librmcs/ecat/pd_stream.hpp>

#include "rmcs_pd.h"
#include "xcore_channel.hpp"

// Hybrid fixed-PDO variant of the core0 process-data glue (RMCS_ECAT_HYBRID_PD).
// The 352-byte PDO is split into two regions moved together on every mapping
// pass (see librmcs/ecat/hybrid_pd.hpp):
//
//   offset   0..335  cyclic region -- seven 12-byte slots per CAN bus, 28 total
//                                    (latest-wins, seq-gated per slot).
//   offset 336..351  stream chunk  -- one 16-byte pd_stream ARQ chunk, handled
//                  exactly as pd_glue.cpp does (the protocol/session byte stream
//                  over the down / up rings, with USB co-transport arbitration).
//
// The two regions use independent cross-core ring pairs, so the raw CAN data
// path and the protocol stream never contend.

namespace ecat = librmcs::firmware::ecat;
namespace native = librmcs::ecat;

namespace {

static_assert(
    RMCS_PD_CHUNK_SIZE == native::kHybridPdSize,
    "SSC/ESI process data size must equal the hybrid PDO (352 bytes)");
static_assert(
    native::kHybridMailboxRegionSize == native::kHybridMailboxCount * native::kNativeMailboxSize,
    "hybrid fixed region must contain all 28 native-format slots");
static_assert(
    native::kHybridStreamChunkSize == librmcs::ecat::kHybridPdChunkSize,
    "hybrid stream chunk must equal the 12-byte-payload pd_stream chunk");
static_assert(
    native::kHybridMailboxCount * native::kNativeRecordSize <= ecat::kXcoreMailboxDownRingSize,
    "one complete hybrid downlink batch must fit the cross-core ring");

// Byte offsets of the two regions inside the PDO image.
constexpr std::size_t kMailboxOffset = native::kHybridMailboxRegionOffset;
constexpr std::size_t kStreamOffset = native::kHybridStreamRegionOffset;

ecat::XcoreChannel* channel = nullptr;
librmcs::ecat::HybridPdStreamEndpoint endpoint;

// Fixed-region state: last downlink freshness counter forwarded per slot, the
// latched uplink slot image, and an independent uplink generation per slot.
// Keeping the generation outside up_image lets a USB handback clear stale data
// without restarting seq at 1 while the EtherCAT master remains continuously OP.
std::uint8_t last_down_seq[native::kHybridMailboxCount] = {};
std::uint8_t up_image[native::kHybridMailboxRegionSize] = {};
std::uint8_t up_seq[native::kHybridMailboxCount] = {};

// Stream transport arbitration, identical to pd_glue.cpp: when USB owns the
// stream the complete ESC process-data path goes inert. The ownership bit is
// also published in XcoreChannel so the core1 CAN ISR routes feedback back into
// the serializer for the USB host instead of the EtherCAT-only fixed slots.
std::atomic<bool> usb_active{false};

void drain_uplink_records() {
    std::byte buffer[native::kNativeRecordSize * 8];
    for (;;) {
        const std::size_t got = channel->mailbox_up.pop(buffer);
        if (got == 0)
            break;
        for (std::size_t offset = 0; offset + native::kNativeRecordSize <= got;
             offset += native::kNativeRecordSize) {
            const auto* record = reinterpret_cast<const std::uint8_t*>(buffer + offset);
            const std::uint16_t current_epoch =
                static_cast<std::uint16_t>(channel->link_epoch.load(std::memory_order::acquire));
            if (native::native_record_epoch(record) != current_epoch)
                continue;
            const std::uint8_t bus = record[native::kNativeRecordBusOffset];
            const std::uint8_t slot = record[native::kNativeRecordSlotOffset];
            if (bus >= native::kNativeBusCount || slot >= native::kHybridSlotsPerBus)
                continue;
            const std::size_t mailbox_index = native::hybrid_mailbox_index(bus, slot);
            std::uint8_t* mailbox = up_image + mailbox_index * native::kNativeMailboxSize;
            std::memcpy(
                mailbox + native::kNativeMetaOffset,
                record + native::kNativeRecordMailboxOffset + native::kNativeMetaOffset,
                native::kNativeMailboxSize - native::kNativeMetaOffset);
            up_seq[mailbox_index] = static_cast<std::uint8_t>(up_seq[mailbox_index] + 1);
            if (up_seq[mailbox_index] == 0)
                up_seq[mailbox_index] = 1;
            mailbox[native::kNativeSeqOffset] = up_seq[mailbox_index];
        }
    }
}

void discard_uplink_records() {
    std::byte buffer[native::kNativeRecordSize * 8];
    while (channel->mailbox_up.pop(buffer) != 0) {}
}

void enter_fixed_ownership(bool reset_slot_sequences) {
    // Ask core1 to quiesce the fixed producer before clearing consumer state.
    // The epoch below is authoritative if core1 misses this short-lived flag.
    channel->usb_active.store(1U, std::memory_order::release);
    usb_active.store(true, std::memory_order::release);
    // Advance before purging: records queued before this transition are stale
    // even if core1 never observes the short-lived ownership flag or an ISR
    // completes its ring push after the purge.
    channel->link_epoch.fetch_add(1, std::memory_order::acq_rel);
    discard_uplink_records();
    std::memset(up_image, 0, sizeof(up_image));
    if (reset_slot_sequences) {
        std::memset(last_down_seq, 0, sizeof(last_down_seq));
        // SAFEOP -> OP is a new EtherCAT generation on both ends. USB
        // handback passes false and deliberately preserves up_seq because WKC
        // stayed complete and the host still remembers the previous values.
        std::memset(up_seq, 0, sizeof(up_seq));
    }

    endpoint.reset();
    channel->usb_active.store(0U, std::memory_order::release);
    usb_active.store(false, std::memory_order::release);
}

} // namespace

extern "C" {

void rmcs_pd_init(void) { channel = &ecat::xcore_channel_init(); }

void rmcs_pd_reset(void) {
    // SAFEOP -> OP is a new fixed session. Block the producer and purge any
    // pre-OP feedback before publishing EtherCAT ownership; unlike a USB
    // handback, both fixed-direction sequence histories restart here.
    enter_fixed_ownership(true);
}

void rmcs_pd_on_outputs(const uint8_t* pd) {
    if (usb_active.load(std::memory_order::acquire))
        return;

    // Fixed region: assemble every changed slot from this PDO snapshot, then
    // publish the complete batch with one all-or-nothing ring operation. A
    // failed push consumes no seq, so the entire batch is retried next cycle.
    std::uint8_t records[native::kHybridMailboxCount * native::kNativeRecordSize];
    std::size_t record_count = 0;
    const std::uint16_t link_epoch =
        static_cast<std::uint16_t>(channel->link_epoch.load(std::memory_order::acquire));
    const std::uint8_t* mailboxes = pd + kMailboxOffset;
    for (std::size_t mailbox_index = 0; mailbox_index < native::kHybridMailboxCount;
         ++mailbox_index) {
        const std::uint8_t* mailbox = mailboxes + mailbox_index * native::kNativeMailboxSize;
        const std::uint8_t seq = mailbox[native::kNativeSeqOffset];
        if (seq == 0 || seq == last_down_seq[mailbox_index])
            continue;

        std::uint8_t* record = records + record_count * native::kNativeRecordSize;
        record[native::kNativeRecordBusOffset] =
            static_cast<std::uint8_t>(mailbox_index / native::kHybridSlotsPerBus);
        record[native::kNativeRecordSlotOffset] =
            static_cast<std::uint8_t>(mailbox_index % native::kHybridSlotsPerBus);
        native::native_record_set_epoch(record, link_epoch);
        std::memcpy(
            record + native::kNativeRecordMailboxOffset, mailbox, native::kNativeMailboxSize);
        ++record_count;
    }

    const std::size_t batch_size = record_count * native::kNativeRecordSize;
    const std::span<const std::byte> batch{reinterpret_cast<const std::byte*>(records), batch_size};
    if (!batch.empty() && channel->mailbox_down.try_push(batch)) {
        for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
            const std::uint8_t* record = records + record_index * native::kNativeRecordSize;
            const std::size_t mailbox_index =
                record[native::kNativeRecordBusOffset] * native::kHybridSlotsPerBus
                + record[native::kNativeRecordSlotOffset];
            last_down_seq[mailbox_index] =
                record[native::kNativeRecordMailboxOffset + native::kNativeSeqOffset];
        }
    }

    // Stream chunk: feed the independent reliable endpoint.
    endpoint.on_peer_chunk(reinterpret_cast<const std::byte*>(pd + kStreamOffset), channel->down);
}

void rmcs_pd_build_inputs(uint8_t* pd) {
    if (usb_active.load(std::memory_order::acquire)) {
        std::memset(pd, 0, native::kHybridPdSize);
        return;
    }

    // Fixed region: latch fresh uplink records, then publish the image.
    drain_uplink_records();
    std::memcpy(pd + kMailboxOffset, up_image, native::kHybridMailboxRegionSize);

    // Stream chunk: build the independent reliable image.
    std::byte* stream = reinterpret_cast<std::byte*>(pd + kStreamOffset);
    endpoint.build_own_chunk(stream, channel->up);
}

bool rmcs_pd_uplink_pending(void) {
    if (usb_active.load(std::memory_order::acquire))
        return false;
    // Fresh CAN frames pending for the fixed region always warrant a refresh.
    if (channel->mailbox_up.readable() != 0)
        return true;
    return endpoint.ready_to_advance() && channel->up.readable() != 0;
}

size_t rmcs_pd_downlink_free(void) { return ecat::kXcoreDownRingSize - channel->down.readable(); }

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
    const bool was = usb_active.load(std::memory_order::acquire);
    if (was == active)
        return;

    if (active) {
        // Stop the core1 fixed path first. Any downlink records that were
        // already queued are still consumed there, but discarded while this
        // cross-core ownership flag is set.
        channel->usb_active.store(1U, std::memory_order::release);
        usb_active.store(true, std::memory_order::release);
        channel->link_epoch.fetch_add(1, std::memory_order::acq_rel);
    } else {
        // Preserve downlink history across USB ownership: an unchanged stale
        // ESC output must not become a fresh CAN command at handback.
        enter_fixed_ownership(false);
        return;
    }

    endpoint.reset();
}

bool rmcs_pd_usb_active(void) { return usb_active.load(std::memory_order::acquire); }

} // extern "C"
