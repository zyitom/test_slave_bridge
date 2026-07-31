#pragma once

// Process-data host transport of the core-swap layout: the core0 end of the
// SHARE_RAM rings (ecat/CORE_SWAP_MIGRATION.md section 4, step 1).
//
// Shape ported verbatim from ecat/core1/src/host_link.hpp, which is the same
// transport seen from the other side of the swap: downlink bytes arrive from the
// `down` ring (fed by core1's stop-and-wait ARQ, which makes the EtherCAT PDO
// image lossless and ordered), uplink batches are pushed into `up`
// all-or-nothing. Session lifecycle and downlink dispatch come from the shared
// link::HostSession, so the host handshake is byte-identical to the USB
// transport.
//
// Step 1 hard-codes "EtherCAT always owns the data plane": this is the ONLY live
// link::HostSession instance, so link::uplink_serializer() / uplink_enabled()
// resolve to it (xcore/pd_link.cpp) and the CAN/UART drivers reach it unchanged.
// Merging this with usb::Vendor into one instance with two transmit backends is
// migration step 4; do not anticipate it here.

#if defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include <class/vendor/vendor_device.h>
#include <device/usbd.h>

#include "core/src/utility/assert.hpp"
#include "firmware/rmcs_board/app/src/link/host_session.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"
#include "firmware/rmcs_board/ecat/common/xcore_channel.hpp"

namespace librmcs::firmware::xcore {

class PdLink : public link::HostSession {
public:
    using Lazy = utility::Lazy<PdLink>;

    PdLink() = default;

    // core1 asked for the data plane back (ecat_claim, bumped on SAFEOP -> OP).
    // Session policy, identical to the pre-swap fieldbus side: drop the session
    // and any partially deserialized frame, then let the host re-handshake on
    // top of the fresh ARQ stream. Stale uplink batches are cleared on the next
    // kStart by HostSession::activate_session().
    //
    // NOT called on ring pops: the PD stream has no transfer boundaries, so
    // finish_downlink_transfer() must happen at link restart and nowhere else
    // (ecat/CORE_SWAP_MIGRATION.md section 4, step 4 trap).
    void handle_link_restart() {
        finish_downlink_transfer();
        deactivate_session();
    }

    // Main loop: pump serialized uplink batches into the up ring.
    // XcoreRing::try_push is all-or-nothing, so a batch that does not fit stays
    // pending until core1's ARQ drains the ring -- end-to-end backpressure,
    // nothing is dropped. Returns true iff a batch was published this call, so
    // the caller rings the cross-core doorbell exactly once per publication.
    bool try_transmit(ecat::XcoreRing<ecat::kXcoreUpRingSize>& up_ring) {
        const auto* batch = next_batch();
        if (!batch)
            return false;

        if (up_ring.try_push(batch->data())) {
            finish_batch();
            return true;
        }

        return false;
    }

    // USB backend of the same session (migration step 4). The two transports are
    // mutually exclusive -- never concurrent -- so one HostSession with two
    // transmit shapes is the whole merge: the protocol stack, the deserializer
    // and the batch pool are shared, and link::uplink_serializer() keeps
    // resolving here, which is why the CAN/UART drivers never learn that a
    // second transport exists.
    //
    // Shape ported from usb::Vendor::try_transmit: packetize the batch at the
    // endpoint's max packet size and finish it with a short packet or a ZLP, so
    // the host sees a transfer boundary. transmitted_size_ carries the partial
    // progress across calls.
    bool try_transmit_usb() {
        const auto* batch = next_batch();
        if (!batch)
            return false;

        if (!tud_vendor_n_write_available(0))
            return false;

        const auto data = batch->data();
        const std::size_t max_packet_size = (tud_speed_get() == TUSB_SPEED_HIGH) ? 512 : 64;
        const auto target_size = std::min(data.size() - transmitted_size_, max_packet_size);

        if (target_size) {
            const auto* src = reinterpret_cast<const uint8_t*>(data.data() + transmitted_size_);
            core::utility::assert_debug(tud_vendor_n_write(0, src, target_size) == target_size);
        } else {
            core::utility::assert_debug(tud_vendor_n_write_zlp(0));
        }

        transmitted_size_ += target_size;
        if (transmitted_size_ == data.size() && target_size < max_packet_size) {
            finish_batch();
            transmitted_size_ = 0;
        }

        return true;
    }

    // USB downlink. Unlike the PD stream, USB HAS transfer boundaries: a short
    // packet ends the transfer, which is where framing recovery may resync.
    void handle_usb_downlink(std::span<const std::byte> buffer, bool finished) {
        link::HostSession::handle_downlink(buffer);
        if (finished)
            finish_downlink_transfer();
    }

private:
    // A new nonce reset the stream: the in-flight batch was released, so the USB
    // backend's partial-transfer progress must go with it.
    void session_activated_callback() override { transmitted_size_ = 0; }

    std::size_t transmitted_size_ = 0;
};

inline constinit PdLink::Lazy pd_link;

// Construct the link. Must precede every driver init(): the CAN/UART ISRs
// serialize through link::uplink_serializer(), which resolves here.
inline void pd_link_init() { pd_link.init(); }

// One main-loop pass of the cross-core data plane: honour a pending core1 claim,
// drain the down ring into the deserializer, publish one uplink batch. Lives in
// pd_link.cpp so app.cpp carries no ring details, and is a cheap two-load no-op
// while core1 has no process data to move.
void pump_data_plane();

// Called from tud_vendor_rx_cb (USB ISR) when a host sends on the vendor OUT
// endpoint: records the claim, nothing more. The main loop performs the actual
// handover, because it clears the batch pool and InterruptSafeBuffer::clear()
// must not be re-entered from interrupt context.
void notify_usb_activity();

// True while USB drives the protocol stack. Read by the USB downlink callback,
// which must not feed the deserializer once EtherCAT has taken the plane back.
bool usb_owns_data_plane();

} // namespace librmcs::firmware::xcore

#else

namespace librmcs::firmware::xcore {

// Single-core builds have no cross-core data plane; usb::Vendor is the host
// transport and defines link::uplink_serializer() itself. These no-ops keep
// app.cpp free of build-configuration branches, matching secondary_core.hpp.
inline void pd_link_init() {}
inline void pump_data_plane() {}

} // namespace librmcs::firmware::xcore

#endif
