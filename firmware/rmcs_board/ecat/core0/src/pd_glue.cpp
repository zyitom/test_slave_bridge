#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <librmcs/ecat/pd_stream.hpp>

#include "rmcs_pd.h"
#include "xcore_channel.hpp"

namespace ecat = librmcs::firmware::ecat;

namespace {

static_assert(
    RMCS_PD_CHUNK_SIZE == librmcs::ecat::kPdChunkSize,
    "SSC/ESI process data size and the stream chunk layout must agree");

// Core0-local state. The channel pointer is set once by rmcs_pd_init()
// before core1 release and before the SSC stack starts calling the hooks.
ecat::XcoreChannel* channel = nullptr;
librmcs::ecat::PdStreamEndpoint endpoint;

// Transport arbitration flag. Read from the ESC PDO hooks (SSC main loop / PDI
// ISR) and the uplink doorbell, written from the USB context; a plain atomic
// makes those single-word accesses coherent on core0. When set, the ESC hooks
// are inert and the USB shuttle owns the rings -- see rmcs_pd.h.
std::atomic<bool> usb_active{false};

} // namespace

extern "C" {

void rmcs_pd_init(void) { channel = &ecat::xcore_channel_init(); }

void rmcs_pd_reset(void) {
    endpoint.reset();
    // A fresh OP cycle means an EtherCAT master is (re)claiming the link, so
    // hand the rings back from any prior USB owner to the ESC path.
    usb_active.store(false, std::memory_order::release);
    channel->usb_active.store(0, std::memory_order::release);
    // Ring contents are intentionally left untouched: core1 owns one end of
    // each ring, so clearing them from here would race. What survives an OP
    // cycle is a protocol-session decision made in later phases (README,
    // "session policy"); the epoch below lets core1 observe the restart.
    channel->link_epoch.fetch_add(1, std::memory_order::release);
}

void rmcs_pd_on_outputs(const uint8_t* pd) {
    // USB owns the rings: ignore the ESC output image so the downlink ring
    // keeps a single producer.
    if (usb_active.load(std::memory_order::acquire))
        return;
    endpoint.on_peer_chunk(reinterpret_cast<const std::byte*>(pd), channel->down);
}

void rmcs_pd_build_inputs(uint8_t* pd) {
    // USB owns the uplink ring: publish an idle chunk (seq 0 is ignored by the
    // peer) without consuming it, keeping a single consumer.
    if (usb_active.load(std::memory_order::acquire)) {
        std::memset(pd, 0, RMCS_PD_CHUNK_SIZE);
        return;
    }
    endpoint.build_own_chunk(reinterpret_cast<std::byte*>(pd), channel->up);
}

bool rmcs_pd_uplink_pending(void) {
    if (usb_active.load(std::memory_order::acquire))
        return false;
    // Racy against the PDI ISR by design: both operands are single-word
    // reads, and a stale answer only skips or adds one poll -- the
    // authoritative re-check runs inside build_own_chunk() with the ESC
    // interrupt masked by the caller.
    return endpoint.ready_to_advance() && channel->up.readable() != 0;
}

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
    const bool was = usb_active.exchange(active, std::memory_order::acq_rel);
    channel->usb_active.store(active ? 1U : 0U, std::memory_order::release);
    if (was == active)
        return;
    // Handover between transports: reset the ARQ endpoint (stale on the USB
    // side, must be clean when EtherCAT takes back over) and let core1 observe
    // the restart. Rings are left in place; the session layer re-syncs.
    endpoint.reset();
    channel->link_epoch.fetch_add(1, std::memory_order::release);
}

bool rmcs_pd_usb_active(void) { return usb_active.load(std::memory_order::acquire); }

} // extern "C"
