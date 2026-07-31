/*
 * ARQ endpoint and ring hand-off for the EtherCAT core. See ecat_pd.h for why
 * putting a cross-core ring between the ARQ and the protocol stack preserves
 * exactly-once delivery -- that argument is the reason this file is allowed to
 * exist, so read it before changing anything here.
 */

#include "ecat_pd.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <librmcs/ecat/pd_stream.hpp>

#include "ecat_xcore.h"
#include "xcore_channel.hpp"

namespace {

namespace ecat = librmcs::firmware::ecat;

static_assert(
    RMCS_PD_CHUNK_SIZE == librmcs::ecat::kPdChunkSize,
    "SSC/ESI process data size and the stream chunk layout must agree");

// Core1-local. Bound once by ecat_pd_init() before MainInit(), read-only
// afterwards; the channel contents carry all cross-core ordering.
ecat::XcoreChannel* channel = nullptr;
librmcs::ecat::PdStreamEndpoint endpoint;

} // namespace

extern "C" {

void ecat_pd_init(void) { channel = ecat_xcore_channel(); }

void ecat_pd_reset(void) {
    endpoint.reset();

    // Drop stale uplink from a previous OP interval. Without this, bytes that
    // were popped into the ARQ window but never acknowledged would be replayed
    // into the new session as if they belonged to it.
    //
    // Safe to do here and ONLY here: core1 is the consumer of `up`, so this
    // advances our own out_ index and never races core0's in_. The mirror rule
    // applies to core0 for `down`.
    std::byte discard[64];
    while (channel->up.pop(discard) != 0) {}

    // Ask core0 for the data plane. Release ordering pairs with core0's acquire
    // load, so the endpoint reset and the ring drain above are visible before
    // core0 acts on the claim.
    channel->ecat_claim.fetch_add(1, std::memory_order::release);
}

void ecat_pd_on_outputs(const uint8_t* pd) {
    endpoint.on_peer_chunk(reinterpret_cast<const std::byte*>(pd), channel->down);
}

void ecat_pd_build_inputs(uint8_t* pd) {
    endpoint.build_own_chunk(reinterpret_cast<std::byte*>(pd), channel->up);
}

bool ecat_pd_uplink_pending(void) {
    return endpoint.ready_to_advance() && channel->up.readable() != 0;
}

} // extern "C"
