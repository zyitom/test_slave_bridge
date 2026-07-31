#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_PD_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_PD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Process-data plane of the EtherCAT core: the stop-and-wait ARQ endpoint that
 * turns the SyncManager's latest-wins PDO image into an exactly-once byte
 * stream, plus the two ring hand-offs to core0.
 *
 * This is the core-swap counterpart of what core0/src/pd_glue.cpp used to do.
 * The endpoint now sits on the SAME core as the ESC (transport layer next to the
 * wire) while the protocol stack sits on core0 next to CAN/UART (application
 * layer next to the data source) -- see ../../CORE_SWAP_MIGRATION.md 3.1.
 *
 * Ring direction is defined relative to the HOST, so the names did not change:
 *   down = host -> device : WE PRODUCE here (from the ESC output image)
 *   up   = device -> host : WE CONSUME here (into the ESC input image)
 *
 * Why inserting a ring between the ARQ and the protocol stack is safe (the one
 * question worth re-deriving before touching this file):
 *
 *   - Uplink retransmit replays from PdStreamEndpoint's own Slot::payload and
 *     NEVER pops the ring a second time -- the ring's out_ advances once per
 *     new chunk, decoupled from how many times that chunk is re-sent.
 *   - Downlink duplicate/out-of-order chunks are rejected by the seq check
 *     INSIDE on_peer_chunk BEFORE try_push, so the ring's in_ advances once per
 *     accepted seq. A repeated SyncManager image cannot double-push bytes.
 *   - Both the ARQ layer and XcoreRing are exactly-once/ordered/lossless, and
 *     composing two such byte channels yields the same guarantees.
 *
 * So core0's deserializer sees byte-for-byte what it saw when the endpoint was
 * co-located with it. Do not "optimize" the seq check to after the push.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Chunk size of one PDO in each direction. Must equal librmcs::ecat::
 * kPdChunkSize; a static_assert in ecat_pd.cpp enforces it. */
#define RMCS_PD_CHUNK_SIZE (48U)

/* Bind the process-data rings. Call after ecat_xcore_init() (which validates
 * the channel version) and BEFORE MainInit(), so the SSC can never invoke a
 * PDO hook against an unbound endpoint. */
void ecat_pd_init(void);

/* SAFEOP -> OP. Resets the ARQ endpoint and drains the ring we consume, then
 * asks core0 for the data plane by bumping ecat_claim.
 *
 * Flush rule: each core flushes ONLY the ring it consumes (up here, down on
 * core0). Each side advances just its own out_ index, which never races the
 * peer's in_ -- that is what makes this safe to do unilaterally.
 *
 * The claim is a REQUEST, not a seizure: core0 owns the arbitration decision
 * because the USB events that compete for the link arrive there. In step 1
 * core0 always grants it. */
void ecat_pd_reset(void);

/* PDO hooks, called from the SSC (PDI ISR context in SM-synchron mode). */
void ecat_pd_on_outputs(const uint8_t* pd);
void ecat_pd_build_inputs(uint8_t* pd);

/* True when a fresh uplink chunk could be published right now. Deliberately
 * racy against the PDI ISR: both operands are single-word reads and a stale
 * answer only skips or adds one poll. The authoritative re-check happens inside
 * build_own_chunk() with the ESC interrupt masked by the caller.
 *
 * up.readable() is only exact on the CONSUMER side -- which is this core after
 * the swap, so the original reasoning survives verbatim. */
bool ecat_pd_uplink_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_PD_H */
