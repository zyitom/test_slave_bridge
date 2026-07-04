#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE0_SRC_RMCS_PD_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE0_SRC_RMCS_PD_H

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
 */
#define RMCS_PD_CHUNK_SIZE (128U)

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

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE0_SRC_RMCS_PD_H */
