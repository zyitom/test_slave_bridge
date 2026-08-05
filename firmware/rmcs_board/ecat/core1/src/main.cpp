#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <board.h>
#include <hpm_mbx_drv.h>

#include "xcore_channel.hpp"

namespace {

// Cross-core uplink doorbell (core1 -> core0). After core1 publishes a
// telemetry batch into the up ring, it pokes HPM_MBX0B; core0 takes the
// matching HPM_MBX0A interrupt and maps the reply into the ESC input image at
// once, instead of waiting for the next SSC MainLoop pass -- collapsing the
// produce-to-ESC turnaround from that loop's slow-path granularity (tens of
// us) to interrupt latency. See ecat_appl.c (rmcs_uplink_doorbell_isr) and
// ../README.md ("uplink refresh").
//
// core0 owns the shared MBX0 clock (rmcs_uplink_doorbell_init runs before this
// core is released), so here only the HPM_MBX0B port needs resetting.
[[maybe_unused]] void uplink_doorbell_init() { mbx_init(HPM_MBX0B); }

[[maybe_unused]] void uplink_doorbell_ring() {
    // Publish the ring bytes BEFORE the doorbell. XcoreRing::try_push ends in a
    // release store, which orders the payload before the index but does NOT
    // order that non-cacheable store ahead of the following device-register
    // write on RISC-V. A full fence (memory + I/O, both directions) guarantees
    // core0 observes the pushed bytes once it sees the mailbox word.
    __asm__ volatile("fence" ::: "memory");
    // Send failure means a poke is already pending in the single-word mailbox;
    // ignore it -- one interrupt is enough, and the up ring is the source of
    // truth the handler re-reads.
    (void)mbx_send_message(HPM_MBX0B, 0);
}

} // namespace

// Core1: the fieldbus core of the EtherCAT stream bridge.
//
// Default build: the librmcs protocol application -- deserialize the host
// command stream from the down ring into the CAN/UART drivers, serialize
// their uplink back into the up ring, with the same session handshake as the
// USB firmware (see host_link.hpp).
//
// -DRMCS_ECAT_CORE1_LOOPBACK=1 restores the P1 bring-up image: a lossless
// byte echo with zero peripheral code, which the host tool
// ecat_stream_latency uses for byte-exact link validation and RTT scans.

#if defined(RMCS_ECAT_NATIVE_CAN) && RMCS_ECAT_NATIVE_CAN

# include <cstring>

# include <hpm_dma_mgr.h>
# include <hpm_l1c_drv.h>

# include <librmcs/ecat/native_can.hpp>

# include "core/include/librmcs/data/datas.hpp"
# include "firmware/rmcs_board/app/src/can/can.hpp"
# include "firmware/rmcs_board/app/src/led/led.hpp"
# include "firmware/rmcs_board/app/src/timer/timer.hpp"
# include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"

// Core1, native CAN variant: no protocol, no session. The 48-byte PDO is four
// per-bus CAN mailboxes (native_can.hpp); core0 hands us downlink frames as
// 16-byte records and latches our uplink records back into the input image.
// This loop forwards records to the CAN drivers and polls RX FIFO0 back out,
// which is the whole point of the native path: no ARQ, no serializer.
int main() {
    using namespace librmcs::firmware; // NOLINT(google-build-using-namespace)
    namespace native = librmcs::ecat;
    namespace data = librmcs::data;

    {
        const utility::InterruptLockGuard guard;

        board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
        uplink_doorbell_init();
        dma_mgr_init();
        l1c_dc_enable_writearound();

        led::led.init();
        timer::timer.init();

        for (auto& board_can : can::can_array)
            board_can.init();
    }

    auto& channel = ecat::xcore_channel_wait();
    uint32_t last_tick = 0;

    std::byte down_buffer[native::kNativeRecordSize * 16];
    while (true) {
        // Host -> CAN: forward each downlink record to its bus.
        const std::size_t received = channel.down.pop(down_buffer);
        for (std::size_t offset = 0; offset + native::kNativeRecordSize <= received;
             offset += native::kNativeRecordSize) {
            const auto* record = reinterpret_cast<const std::uint8_t*>(down_buffer + offset);
            const std::uint8_t bus = record[native::kNativeRecordBusOffset];
            if (bus >= can::kCanCount)
                continue;
            const std::uint8_t* mailbox = record + native::kNativeRecordMailboxOffset;
            const std::uint8_t meta = mailbox[native::kNativeMetaOffset];
            const std::uint8_t length = native::native_meta_length(meta);
            if (length > native::kNativeMaxDataSize)
                continue;
            const std::uint32_t can_id =
                static_cast<std::uint32_t>(mailbox[native::kNativeIdOffset])
                | static_cast<std::uint32_t>(mailbox[native::kNativeIdOffset + 1]) << 8;
            if (can_id > 0x7FFU)
                continue;

            data::CanDataView view;
            view.is_fdcan = native::native_meta_is_fdcan(meta);
            view.is_extended_can_id = false;
            view.is_remote_transmission = false;
            view.can_id = can_id;
            view.can_data = {
                reinterpret_cast<const std::byte*>(mailbox + native::kNativeDataOffset), length};
            can::can_array[bus]->handle_downlink(view);
        }

        // CAN -> host: drain every bus's RX FIFO into uplink records.
        bool published = false;
        for (std::uint8_t bus = 0; bus < can::kCanCount; ++bus) {
            data::CanDataView view;
            std::uint8_t storage[8];
            while (can::can_array[bus]->read_native(view, storage)) {
                // The native mailbox has no extended/RTR flags. Drop formats
                // it cannot represent instead of silently relabeling them as
                // standard data frames.
                if (view.is_extended_can_id || view.is_remote_transmission
                    || view.can_id > 0x7FFU) {
                    continue;
                }
                std::uint8_t record[native::kNativeRecordSize] = {};
                record[native::kNativeRecordBusOffset] = bus;
                std::uint8_t* mailbox = record + native::kNativeRecordMailboxOffset;
                mailbox[native::kNativeMetaOffset] = native::native_meta(
                    view.is_fdcan, static_cast<std::uint8_t>(view.can_data.size()));
                mailbox[native::kNativeIdOffset] = static_cast<std::uint8_t>(view.can_id);
                mailbox[native::kNativeIdOffset + 1] = static_cast<std::uint8_t>(view.can_id >> 8);
                if (!view.can_data.empty())
                    std::memcpy(
                        mailbox + native::kNativeDataOffset, view.can_data.data(),
                        view.can_data.size());
                while (!channel.up.try_push(
                    std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(record), native::kNativeRecordSize})) {}
                published = true;
            }
        }
        if (published)
            uplink_doorbell_ring();

        const uint32_t tick = timer::timer->tick_count();
        if (tick != last_tick) {
            last_tick = tick;
            led::led->update(tick);
        }
    }
}

#elif defined(RMCS_ECAT_CORE1_LOOPBACK) && RMCS_ECAT_CORE1_LOOPBACK

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    uplink_doorbell_init();

    auto& channel = librmcs::firmware::ecat::xcore_channel_wait();

    std::byte buffer[256];
    while (true) {
        const std::size_t received = channel.down.pop(buffer);
        if (received == 0)
            continue;

        const std::span<const std::byte> chunk{buffer, received};
        // Spin until the uplink ring has room: the echo must be lossless for
        // the end-to-end ARQ test to be meaningful.
        while (!channel.up.try_push(chunk)) {}
        // Wake core0 to publish the echo now, so the RTT scan measures the
        // doorbell path rather than the MainLoop poll granularity.
        uplink_doorbell_ring();
    }

    return 0;
}

#elif defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD

# include <cstring>

# include <hpm_dma_mgr.h>
# include <hpm_l1c_drv.h>

# include <librmcs/ecat/hybrid_pd.hpp>
# include <librmcs/ecat/native_can.hpp>

# include "core/include/librmcs/data/datas.hpp"
# include "firmware/rmcs_board/app/src/can/can.hpp"
# include "firmware/rmcs_board/app/src/led/led.hpp"
# include "firmware/rmcs_board/app/src/timer/timer.hpp"
# include "firmware/rmcs_board/app/src/uart/uart.hpp"
# include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"
# include "host_link.hpp"
# include "hybrid_link.hpp"

// Core1, hybrid fixed-PDO variant: the NORMAL protocol/session loop (the stream
// stays fully alive over the down/up rings) PLUS the native CAN mailbox path.
// Raw CAN data frames flow through the separate mailbox rings -- downlink records
// drained here to the CAN transmit path, uplink records pushed by the CAN RX ISR
// (see hybrid_link.cpp / can.cpp) -- while protocol/config traffic keeps using
// the stream. This is deliberately not the native tight-poll branch: the RX
// interrupt stays enabled (its polling was what made the native path slower).
int main() {
    using namespace librmcs::firmware; // NOLINT(google-build-using-namespace)
    namespace native = librmcs::ecat;
    namespace data = librmcs::data;

    {
        const utility::InterruptLockGuard guard;

        board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
        uplink_doorbell_init();
        dma_mgr_init();
        l1c_dc_enable_writearound();

        led::led.init();
        timer::timer.init();

        // The stream link and the mailbox uplink hook must both exist before the
        // driver ISRs fire. The channel magic is already published by core0 (it
        // releases core1 only after rmcs_pd_init), so xcore_channel_wait() does
        // not block; reading it here is safe because board_init_core1() above
        // already mapped SHARE_RAM non-cacheable. Binding it before can.init()
        // arms the RX ISR guarantees the uplink hook has a live channel.
        ecat::host_link.init();
        ecat::hybrid_link_init(ecat::xcore_channel_wait());

        for (auto& board_can : can::can_array)
            board_can.init();

        for (auto& board_uart : uart::uart_array)
            board_uart.init();
    }

    auto& channel = ecat::xcore_channel_wait();
    uint32_t last_epoch = channel.link_epoch.load(std::memory_order::acquire);
    uint32_t last_tick = 0;
    std::byte down_buffer[256];
    static_assert(ecat::kXcoreMailboxDownRingSize % native::kNativeRecordSize == 0);
    std::byte mailbox_buffer[ecat::kXcoreMailboxDownRingSize];

    while (true) {
        // Apply a transport restart before feeding any bytes from the new
        // epoch into the old session/deserializer state.
        const uint32_t epoch = channel.link_epoch.load(std::memory_order::acquire);
        if (epoch != last_epoch) {
            last_epoch = epoch;
            ecat::host_link->handle_link_restart();
        }

        // Host -> fieldbus (stream): drain the down ring into the deserializer.
        const std::size_t received = channel.down.pop(down_buffer);
        if (received != 0)
            ecat::host_link->handle_downlink({down_buffer, received});

        // Fieldbus -> host (stream): pump serialized batches into the up ring.
        if (ecat::host_link->try_transmit(channel.up))
            uplink_doorbell_ring();

        // Host -> CAN (mailboxes): forward each downlink record to its bus, the
        // same transmit path the native variant uses.
        const std::size_t got = channel.mailbox_down.pop(mailbox_buffer);
        const bool discard_mailboxes = channel.usb_active.load(std::memory_order::acquire) != 0
                                    || !ecat::host_link->session_established();
        for (std::size_t offset = 0; offset + native::kNativeRecordSize <= got;
             offset += native::kNativeRecordSize) {
            // Keep consuming while USB owns the link or the protocol session is
            // down, but discard those records so stale or unauthenticated
            // commands cannot reach CAN later. Re-check ownership and epoch for
            // a handover that occurs partway through this batch.
            const auto* record = reinterpret_cast<const std::uint8_t*>(mailbox_buffer + offset);
            const std::uint16_t record_epoch = native::native_record_epoch(record);
            const std::uint16_t current_epoch =
                static_cast<std::uint16_t>(channel.link_epoch.load(std::memory_order::acquire));
            if (discard_mailboxes || channel.usb_active.load(std::memory_order::acquire) != 0
                || record_epoch != current_epoch) {
                continue;
            }
            const std::uint8_t bus = record[native::kNativeRecordBusOffset];
            if (bus >= can::kCanCount)
                continue;
            const std::uint8_t* mailbox = record + native::kNativeRecordMailboxOffset;
            const std::uint8_t meta = mailbox[native::kNativeMetaOffset];
            const std::uint8_t length = native::native_meta_length(meta);
            if (length > native::kNativeMaxDataSize)
                continue;
            const std::uint32_t can_id =
                static_cast<std::uint32_t>(mailbox[native::kNativeIdOffset])
                | static_cast<std::uint32_t>(mailbox[native::kNativeIdOffset + 1]) << 8;
            if (can_id > 0x7FFU)
                continue;

            data::CanDataView view;
            view.is_fdcan = native::native_meta_is_fdcan(meta);
            view.is_extended_can_id = false;
            view.is_remote_transmission = false;
            view.can_id = can_id;
            view.can_data = {
                reinterpret_cast<const std::byte*>(mailbox + native::kNativeDataOffset), length};
            // Narrow the handover race once more immediately before touching
            // CAN. A record that passed the first check but was superseded
            // while being decoded is dropped instead of crossing ownership.
            const std::uint16_t final_epoch =
                static_cast<std::uint16_t>(channel.link_epoch.load(std::memory_order::acquire));
            if (!ecat::host_link->session_established()
                || channel.usb_active.load(std::memory_order::acquire) != 0
                || record_epoch != final_epoch) {
                continue;
            }
            can::can_array[bus]->handle_downlink(view);
        }

        for (auto& board_uart : uart::uart_array)
            board_uart->try_transmit();

        const uint32_t tick = timer::timer->tick_count();
        if (tick != last_tick) {
            last_tick = tick;
            led::led->set_host_connected(ecat::host_link->session_established());
            led::led->update(tick);
        }
    }
}

#else

# include <hpm_dma_mgr.h>
# include <hpm_l1c_drv.h>

# include "firmware/rmcs_board/app/src/can/can.hpp"
# include "firmware/rmcs_board/app/src/led/led.hpp"
# include "firmware/rmcs_board/app/src/timer/timer.hpp"
# include "firmware/rmcs_board/app/src/uart/uart.hpp"
# include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"
# include "host_link.hpp"

int main() {
    using namespace librmcs::firmware; // NOLINT(google-build-using-namespace)

    {
        const utility::InterruptLockGuard guard;

        board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
        uplink_doorbell_init();
        dma_mgr_init();

        // Same rationale as the USB app: streaming writes bypass D-cache
        // allocation, keeping it available for hot control structures.
        l1c_dc_enable_writearound();

        led::led.init();
        timer::timer.init();

        // The link must exist before the driver ISRs can serialize into it.
        ecat::host_link.init();

        for (auto& can : can::can_array)
            can.init();

        for (auto& board_uart : uart::uart_array)
            board_uart.init();
    }

    auto& channel = ecat::xcore_channel_wait();

    uint32_t last_epoch = channel.link_epoch.load(std::memory_order::acquire);
    uint32_t last_tick = 0;
    std::byte down_buffer[256];

    while (true) {
        // Host -> fieldbus: drain the down ring into the deserializer.
        const std::size_t received = channel.down.pop(down_buffer);
        if (received != 0)
            ecat::host_link->handle_downlink({down_buffer, received});

        // SAFEOP -> OP re-entry on core0 restarts the PD stream; drop the
        // session so the host re-handshakes (session policy, see host_link).
        const uint32_t epoch = channel.link_epoch.load(std::memory_order::acquire);
        if (epoch != last_epoch) {
            last_epoch = epoch;
            ecat::host_link->handle_link_restart();
        }

        // Fieldbus -> host: pump serialized batches into the up ring, and poke
        // core0 the instant one lands so it maps the reply into the ESC without
        // waiting for the next SSC MainLoop pass.
        if (ecat::host_link->try_transmit(channel.up))
            uplink_doorbell_ring();

        for (auto& board_uart : uart::uart_array)
            board_uart->try_transmit();

        // LED bookkeeping at the 1 kHz tick pace, out of ISR context (same
        // reasoning as the USB app main loop).
        const uint32_t tick = timer::timer->tick_count();
        if (tick != last_tick) {
            last_tick = tick;
            led::led->set_host_connected(ecat::host_link->session_established());
            led::led->update(tick);
        }
    }
}

#endif
