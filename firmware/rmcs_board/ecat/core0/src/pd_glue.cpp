#include <atomic>
#include <cstddef>
#include <cstdint>

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

} // namespace

extern "C" {

void rmcs_pd_init(void) { channel = &ecat::xcore_channel_init(); }

void rmcs_pd_reset(void) {
    endpoint.reset();
    // Ring contents are intentionally left untouched: core1 owns one end of
    // each ring, so clearing them from here would race. What survives an OP
    // cycle is a protocol-session decision made in later phases (README,
    // "session policy"); the epoch below lets core1 observe the restart.
    channel->link_epoch.fetch_add(1, std::memory_order::release);
}

void rmcs_pd_on_outputs(const uint8_t* pd) {
    endpoint.on_peer_chunk(reinterpret_cast<const std::byte*>(pd), channel->down);
}

void rmcs_pd_build_inputs(uint8_t* pd) {
    endpoint.build_own_chunk(reinterpret_cast<std::byte*>(pd), channel->up);
}

bool rmcs_pd_uplink_pending(void) {
    // Racy against the PDI ISR by design: both operands are single-word
    // reads, and a stale answer only skips or adds one poll -- the
    // authoritative re-check runs inside build_own_chunk() with the ESC
    // interrupt masked by the caller.
    return endpoint.ready_to_advance() && channel->up.readable() != 0;
}

} // extern "C"
