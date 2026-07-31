#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE0_SRC_RMCS_PD_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE0_SRC_RMCS_PD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C boundary between the Beckhoff SSC application layer (plain C, see
 * ecat_appl.c) and the C++ stream/ring glue (pd_glue.cpp). Keeping the
 * SSC-facing translation units pure C avoids any dependency on the SSC
 * headers being C++-clean.
 *
 * Process data image size per direction. MUST equal the byte-array PDO size
 * configured in the SSC Tool project and the ESI file (see ../README.md);
 * pd_glue.cpp statically asserts it against the protocol constants.
 *
 * Hybrid fixed-PDO variant (RMCS_ECAT_HYBRID_PD): 352 bytes = 28 x 12-byte
 * cyclic CAN slots + a 16-byte pd_stream chunk (see
 * librmcs/ecat/hybrid_pd.hpp and hybrid_glue.cpp). Stock and native variants
 * keep 48.
 */
#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD
#define RMCS_PD_CHUNK_SIZE (352U)
#else
#define RMCS_PD_CHUNK_SIZE (48U)
#endif

/* Construct and publish the shared-memory channel. Must be called BEFORE
 * releasing core1 and before any other rmcs_pd_* function. */
void rmcs_pd_init(void);

/* SAFEOP -> OP (re)entry: reset the stop-and-wait ARQ endpoint and bump the
 * link epoch so the fieldbus core can observe the (re)start. */
void rmcs_pd_reset(void);

/* SSC PDO mapping hooks. Called from whatever context the SSC dispatches
 * them in (main loop in free-run, PDI ISR in SM-synchron mode); both are
 * safe there. pd points at RMCS_PD_CHUNK_SIZE bytes. */
void rmcs_pd_on_outputs(const uint8_t* pd);
void rmcs_pd_build_inputs(uint8_t* pd);

/* True when a rebuild of the input chunk would stage fresh uplink payload:
 * the in-flight chunk (if any) is acknowledged AND the cross-core uplink
 * ring holds bytes. Cheap peek used by the doorbell ISR and the SSC main-loop
 * hook to publish fieldbus replies to the ESC without waiting for the next
 * PDI event (see ecat_appl.c, rmcs_input_refresh). */
bool rmcs_pd_uplink_pending(void);

/* Arm the cross-core uplink doorbell (HPM_MBX0A RX interrupt) on core0. Must
 * be called AFTER rmcs_pd_init() and BEFORE core1 is released: it enables the
 * shared MBX0 clock the fieldbus core needs to poke HPM_MBX0B. Implemented in
 * ecat_appl.c alongside the handler. */
void rmcs_uplink_doorbell_init(void);

/* Mask/unmask the doorbell IRQ. Thread-context code that runs the vendor data
 * pump must wrap the call, so the pump inside the doorbell ISR cannot preempt
 * it (implemented in ecat_appl.c next to the ISR that owns the IRQ number). */
void rmcs_uplink_doorbell_set_enabled(bool enabled);

/*
 * USB transport path (usb_runtime.cpp). The same cross-core rings carry the
 * raw protocol byte stream regardless of transport, so USB does not need the
 * ARQ endpoint: USB is already reliable and in-order. These helpers let the
 * USB byte-shuttle move bytes directly between the bulk endpoints and the
 * rings. All are core0-only and expect the arbitration below to guarantee the
 * ESC hooks are inert while USB owns the rings (single-producer / single-
 * consumer per ring is preserved).
 */

/* Free space (bytes) in the host->device (downlink) ring right now. The USB
 * shuttle reads at most this many bytes out of the bulk-OUT FIFO so a push
 * never has to be partially dropped. */
size_t rmcs_pd_downlink_free(void);

/* Push host->device bytes into the downlink ring. Returns the number written
 * (len on success, 0 if it would not fit -- callers size against
 * rmcs_pd_downlink_free()). */
size_t rmcs_pd_push_downlink(const uint8_t* data, size_t len);

/* Pop device->host bytes from the uplink ring into buffer (up to capacity).
 * Returns the number copied. */
size_t rmcs_pd_pop_uplink(uint8_t* buffer, size_t capacity);

/* Transport arbitration ("whichever host is active"): the single ring pair is
 * served by EXACTLY ONE transport at a time. When usb_active is true the ESC
 * PDO hooks (rmcs_pd_on_outputs / rmcs_pd_build_inputs) go inert -- outputs are
 * ignored and inputs emit an idle chunk -- and the USB shuttle owns the rings;
 * when false the EtherCAT ARQ endpoint owns them. Toggling resets the ARQ
 * endpoint and bumps the link epoch so the fieldbus core restarts its session
 * on the new transport. rmcs_pd_reset() (SAFEOP->OP) also clears the USB claim,
 * so bringing up an EtherCAT master hands the link back to EtherCAT. */
void rmcs_pd_set_usb_active(bool usb_active);
bool rmcs_pd_usb_active(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE0_SRC_RMCS_PD_H */
