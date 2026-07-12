#pragma once

#include "firmware/rmcs_board/app/src/link/host_session.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"
#include "xcore_channel.hpp"

namespace librmcs::firmware::ecat {

// EtherCAT host transport: the fieldbus analog of usb::Vendor. Downlink bytes
// arrive from the SHARE_RAM down ring (fed by the core0 PD stream, which the
// stop-and-wait ARQ makes lossless and ordered); uplink batches are pushed
// into the up ring all-or-nothing. Session lifecycle and downlink dispatch
// live in the shared link::HostSession, so the handshake is byte-identical to
// the USB transport and the host SDK works unchanged on top of SOEM.
//
// The PD stream has no transfer boundaries (unlike USB), so the deserializer
// is fed continuously via the inherited handle_downlink(); framing recovery
// happens on link restart only.
class HostLink : public link::HostSession {
public:
    using Lazy = utility::Lazy<HostLink>;

    HostLink() = default;

    // Main loop, step 2: core0 bumped link_epoch (SAFEOP -> OP re-entry, i.e.
    // the master restarted the PD stream). Session policy: drop the session
    // and any partially deserialized frame; the host re-handshakes on top of
    // the fresh ARQ stream, and stale uplink batches are cleared on the next
    // kStart. Ring contents are left alone (core0 owns the other end).
    void handle_link_restart() {
        finish_downlink_transfer();
        deactivate_session();
    }

    // Main loop, step 3: pump serialized uplink batches into the up ring.
    // XcoreRing::try_push is all-or-nothing, so a batch that does not fit
    // simply stays pending until the PD stream drains the ring (end-to-end
    // backpressure, nothing is dropped). Returns true iff a batch was pushed
    // this call, so the caller can ring the cross-core doorbell exactly once
    // per publication (see core1/src/main.cpp) instead of on every empty pass.
    bool try_transmit(XcoreRing<kXcoreUpRingSize>& up_ring) {
        const auto* batch = next_batch();
        if (!batch)
            return false;

        if (up_ring.try_push(batch->data())) {
            finish_batch();
            return true;
        }

        return false;
    }
};

inline constinit HostLink::Lazy host_link;

} // namespace librmcs::firmware::ecat
