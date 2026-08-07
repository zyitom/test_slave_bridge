#include "firmware/rmcs_board/app/src/xcore/pd_link.hpp"

#if defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1

# include <atomic>
# include <cstddef>
# include <cstdint>

# include "core/src/protocol/serializer.hpp"
# include "firmware/rmcs_board/app/src/link/uplink.hpp"
# include "firmware/rmcs_board/app/src/xcore/secondary_core.hpp"
# include "firmware/rmcs_board/ecat/common/xcore_channel.hpp"

namespace librmcs::firmware::link {

// Step 1 arbitration is hard-coded to "EtherCAT always owns the data plane"
// (ecat/CORE_SWAP_MIGRATION.md section 4, step 1), so the PD stream is the ONE
// host transport of this image and the CAN/UART driver ISRs serialize through it.
//
// This is what keeps the driver layer at zero diff: can/ and uart/ reach the
// protocol stack only through these two free functions, never through a
// transport type. usb::Vendor is still constructed -- USB enumeration and DFU-RT
// must keep working -- but nothing binds it to the stack, and app/CMakeLists.txt
// detaches its downlink callback as well.
//
// Same predicate choice as the pre-swap fieldbus binding it replaces
// (ecat/core1/src/host_link.cpp): uplink_enabled() rather than
// session_established(), i.e. driver telemetry starts only once the kStart ack
// has been queued.
core::protocol::Serializer& uplink_serializer() { return xcore::pd_link->serializer(); }
// The cross-core PD stream is a single pipe, so CAN shares it as it always has.
core::protocol::Serializer& can_uplink_serializer() { return xcore::pd_link->serializer(); }
bool uplink_enabled() { return xcore::pd_link->uplink_enabled(); }

} // namespace librmcs::firmware::link

namespace librmcs::firmware::xcore {
namespace {

// Bytes moved out of the down ring per main-loop pass.
//
// 64 rather than the ring's full 1024: the ring holds at most one ARQ chunk
// payload (kPdChunkPayloadSize = 44) per EtherCAT cycle with a window of 2, so 64
// clears everything a cycle can produce in a single pop and the copy is one
// cache-line pair on the stack. Sizing it to the ring instead would put a 1 KiB
// buffer on the main-loop frame to serve a steady state that never exceeds 88
// bytes, and would let one pass hand the deserializer a burst large enough to
// delay tud_task() -- backlog is better spread over passes, since the loop has no
// other blocking work and comes back within microseconds.
//
// This is a throughput knob only, never a correctness one: a short pop leaves the
// remainder in the ring for the next pass, and the ring's own 1024 bytes are what
// absorb a core0 stall (~23 EtherCAT cycles).
constexpr std::size_t kDownlinkBytesPerPass = 64;

// Last observed ecat_claim. Core0-local main-loop state; core1 only ever
// increments the shared counter.
std::uint32_t g_last_ecat_claim = 0;

// Data-plane owner, core0-local authority (migration step 4). Both transports
// feed one HostSession, so exactly one of them may drive it at a time.
//
// core0 decides because the USB events that compete for the link arrive here and
// because core0 is the ring end that actually admits bytes into the
// deserializer. Deciding at the point of execution leaves no cross-core window:
// the switch to discard-mode below takes effect immediately, whatever core1 is
// doing at that instant.
bool g_usb_owns = false;

// Set from tud_vendor_rx_cb (USB ISR context) when a host starts talking on the
// vendor OUT endpoint; consumed by the main loop. The handover itself must NOT
// run in the ISR: it calls deactivate_session(), which clears the batch pool,
// and InterruptSafeBuffer::clear() asserts it is not re-entered from an ISR.
std::atomic<bool> g_usb_claim{false};

} // namespace

void notify_usb_activity() { g_usb_claim.store(true, std::memory_order::release); }

bool usb_owns_data_plane() { return g_usb_owns; }

void pump_data_plane() {
    auto* channel_ptr = channel();
    if (channel_ptr == nullptr) [[unlikely]]
        return;
    auto& channel = *channel_ptr;

    // Handle the claim BEFORE draining. core1 bumps ecat_claim on SAFEOP -> OP
    // after resetting its ARQ endpoint, so every byte still in the down ring at
    // that moment belongs to the previous OP interval. Restarting first means the
    // pop below can only ever feed the deserializer bytes from the new interval;
    // doing it the other way round would splice the tail of the old stream onto
    // the head of the new one.
    //
    // Acquire pairs with core1's release fetch_add, so its endpoint reset and up
    // ring drain are visible before we act on the claim.
    // A USB host started talking: take the data plane away from EtherCAT.
    //
    // Done here rather than in the ISR that observed it, because the handover
    // clears the batch pool and InterruptSafeBuffer::clear() must not run in
    // interrupt context.
    if (g_usb_claim.exchange(false, std::memory_order::acquire) && !g_usb_owns) {
        g_usb_owns = true;
        // Announce first, then restart. core1 stops consuming/producing process
        // data once it sees this; until it does, the discard-mode down-ring drain
        // below keeps its in-flight chunks out of the deserializer, so a late
        // observation costs nothing.
        channel.owner.store(
            static_cast<std::uint32_t>(ecat::XcoreOwner::kUsb), std::memory_order::release);
        pd_link->handle_link_restart();
        channel.link_epoch.fetch_add(1, std::memory_order::acq_rel);
    }

    const std::uint32_t ecat_claim = channel.ecat_claim.load(std::memory_order::acquire);
    if (ecat_claim != g_last_ecat_claim) {
        g_last_ecat_claim = ecat_claim;

        // An EtherCAT master reached OP and wants the link. Grant it: a master
        // entering OP is an explicit, human-configured intent, whereas USB
        // ownership is claimed by mere traffic. (The pre-swap firmware had the
        // symmetric problem -- USB keepalive kept stealing the link back from a
        // running master; see the arbitration note in ../README.md.)
        g_usb_owns = false;
        channel.owner.store(
            static_cast<std::uint32_t>(ecat::XcoreOwner::kEcat), std::memory_order::release);
        pd_link->handle_link_restart();

        // Flush rule (xcore_channel.hpp): each core flushes only the ring it
        // CONSUMES. core0 consumes `down`, so draining it here advances only our
        // own out_ and never races core1's in_. The mirror of what core1 does to
        // `up` in ecat_pd_reset().
        std::byte discard[kDownlinkBytesPerPass];
        while (channel.down.pop(discard) != 0) {}
    }

    // Host -> device: feed the deserializer continuously.
    //
    // A ring pop is NOT a transfer boundary. Unlike USB, where a short packet
    // ends a transfer and lets the deserializer resynchronize, the PD stream is a
    // pure byte stream: popping fewer bytes than requested only means the ring is
    // momentarily empty, not that the host finished sending. Calling
    // finish_downlink_transfer() here would truncate whatever field is mid-parse
    // and desynchronize the stream at an arbitrary offset -- the trap called out
    // in ecat/CORE_SWAP_MIGRATION.md section 4. Framing recovery belongs to
    // handle_link_restart() above, and nowhere else.
    //
    if (g_usb_owns) {
        // USB owns the plane: touch neither ring.
        //
        // This is a latency decision, not a tidiness one. Both rings live in
        // SHARE_RAM, which is mapped non-cacheable, so polling them costs
        // uncached loads on EVERY pass of the loop that also drives the USB
        // uplink. Measured cost of leaving them in: USB p50 124.2 us versus
        // 99.9 us for the single-core image -- almost exactly one 125 us USB
        // microframe, i.e. the extra work pushed the reply past its IN-token
        // slot. Skipping the rings here is what actually delivers the migration's
        // headline number.
        //
        // Safe because the handover above already flushed `down` after
        // announcing the ownership change, and core1 goes inert on process data
        // once it observes `owner` -- so nothing accumulates while USB holds the
        // plane. An EtherCAT master reclaiming the link bumps ecat_claim, which
        // is read above, before this branch.
        (void)pd_link->try_transmit_usb();
        return;
    }

    // Host -> device: feed the deserializer continuously.
    std::byte downlink_buffer[kDownlinkBytesPerPass];
    const std::size_t received = channel.down.pop(downlink_buffer);
    if (received != 0)
        pd_link->handle_downlink({downlink_buffer, received});

    // Device -> host: publish at most one serialized batch per pass. Ring the
    // doorbell only on a successful push, so core1 republishes the ESC input
    // image immediately instead of waiting for its next MainLoop pass. Poking on
    // empty passes would just burn interrupts on core1.
    if (pd_link->try_transmit(channel.up))
        ring_uplink_doorbell();
}

} // namespace librmcs::firmware::xcore

#endif
