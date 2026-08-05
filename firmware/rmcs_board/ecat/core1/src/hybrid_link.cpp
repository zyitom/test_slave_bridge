#include "hybrid_link.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <board.h>
#include <hpm_mbx_drv.h>

#include <librmcs/ecat/hybrid_pd.hpp>
#include <librmcs/ecat/native_can.hpp>

#include "core/include/librmcs/data/datas.hpp"
#include "firmware/rmcs_board/app/src/link/uplink.hpp"
#include "xcore_channel.hpp"

// Hybrid fixed-PDO variant: the raw CAN data path. The CAN RX ISR forwards each
// received frame straight into the native mailbox uplink ring (bypassing the
// protocol serializer, which would duplicate it -- see can.cpp), then rings the
// cross-core doorbell so core0 latches it into the input mailbox region without
// waiting for the next SSC MainLoop pass. The protocol/session byte stream keeps
// flowing over the separate down/up rings in main.cpp.

namespace native = librmcs::ecat;

namespace {

librmcs::firmware::ecat::XcoreChannel* g_channel = nullptr;
std::uint8_t g_next_uplink_slot[native::kNativeBusCount] = {};

} // namespace

namespace librmcs::firmware::ecat {

void hybrid_link_init(XcoreChannel& channel) { g_channel = &channel; }

} // namespace librmcs::firmware::ecat

namespace librmcs::firmware::link {

bool hybrid_fixed_active() {
    return g_channel != nullptr && g_channel->usb_active.load(std::memory_order::acquire) == 0;
}

bool hybrid_can_uplink(std::size_t bus, const data::CanDataView& data) {
    if (g_channel == nullptr || bus >= native::kNativeBusCount || data.is_extended_can_id
        || data.is_remote_transmission || data.can_id > 0x7FFU
        || data.can_data.size() > native::kNativeMaxDataSize) {
        return false;
    }

    const std::uint32_t link_epoch = g_channel->link_epoch.load(std::memory_order::acquire);
    if (g_channel->usb_active.load(std::memory_order::acquire) != 0)
        return false;

    std::uint8_t record[native::kNativeRecordSize] = {};
    record[native::kNativeRecordBusOffset] = static_cast<std::uint8_t>(bus);
    const std::uint8_t slot = g_next_uplink_slot[bus];
    record[native::kNativeRecordSlotOffset] = slot;
    native::native_record_set_epoch(record, static_cast<std::uint16_t>(link_epoch));
    std::uint8_t* mailbox = record + native::kNativeRecordMailboxOffset;

    const std::size_t length = data.can_data.size();
    mailbox[native::kNativeMetaOffset] =
        native::native_meta(data.is_fdcan, static_cast<std::uint8_t>(length));
    mailbox[native::kNativeIdOffset] = static_cast<std::uint8_t>(data.can_id);
    mailbox[native::kNativeIdOffset + 1] = static_cast<std::uint8_t>(data.can_id >> 8);
    if (length != 0)
        std::memcpy(mailbox + native::kNativeDataOffset, data.can_data.data(), length);

    // Revalidate after constructing the record. If ownership changed, the
    // caller falls back to the reliable serializer; if it changes after this
    // check, core0 rejects the record by its old epoch. ISR context never spins.
    if (g_channel->usb_active.load(std::memory_order::acquire) != 0
        || g_channel->link_epoch.load(std::memory_order::acquire) != link_epoch) {
        return false;
    }

    // A full ring also falls back to the reliable stream in can.cpp; advance
    // the round-robin slot only after a successful publish.
    if (!g_channel->mailbox_up.try_push(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(record), native::kNativeRecordSize})) {
        return false;
    }
    g_next_uplink_slot[bus] = static_cast<std::uint8_t>((slot + 1U) % native::kHybridSlotsPerBus);
    return true;
}

void hybrid_uplink_notify() {
    if (g_channel == nullptr)
        return;
    // Order the pushed record ahead of the doorbell write (same fence as the
    // stream doorbell in main.cpp: a non-cacheable release store is not ordered
    // ahead of the following device-register write on RISC-V without it).
    __asm__ volatile("fence" ::: "memory");
    (void)mbx_send_message(HPM_MBX0B, 0);
}

} // namespace librmcs::firmware::link
