#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_DOORBELL_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_DOORBELL_H

/*
 * Uplink doorbell, receiving end (core1).
 *
 * Direction after the core swap: core0 pushes a serialized batch into the up
 * ring and pokes MBX0A; this core takes IRQn_MBX0B and republishes the ESC
 * input image immediately. Before the swap it ran the other way round (core1
 * poked, core0 republished) -- same semantics, the cores traded places.
 *
 * What it buys: in SM-synchron mode the SSC maps inputs only inside the
 * SM2-event ISR, which runs BEFORE core0 has produced the reply to the chunk
 * consumed in that very ISR. Without an out-of-cycle republish the reply waits
 * for the next MainLoop pass, costing a full poll cycle on every
 * request/response exchange.
 *
 * Priority is deliberately BELOW the ESC PDI interrupt so a doorbell can never
 * preempt an in-flight EtherCAT frame; the reverse direction is fenced by
 * DISABLE_ESC_INT() inside the refresh, so the two PDO_InputMapping sites never
 * interleave. The 3-buffer SyncManager swaps atomically, which is what makes the
 * extra write invisible to a mid-read master.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the doorbell. Call after ecat_pd_init() and before MainInit(): once the
 * interrupt is live it may fire and republish at any time.
 *
 * The shared MBX0 clock gate is opened by core0 before this core is released
 * (see app/src/xcore/secondary_core.cpp), so only the local port is reset here.
 * A write to a gated mailbox is silently lost, which is why that ordering
 * matters and is asserted on the core0 side rather than here. */
void ecat_doorbell_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_DOORBELL_H */
