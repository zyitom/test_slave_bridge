/*
 * SSC application layer for the RMCS EtherCAT stream bridge.
 *
 * Mirrors the callback set of the SDK's ecat_io sample (application/
 * digital_io.c) but maps a fixed-size byte-stream chunk instead of digital
 * IO. All real work happens behind the C boundary in rmcs_pd.h; this file
 * stays pure C so it only depends on the Beckhoff SSC headers being
 * C-compilable (which they are by construction).
 */

#include "ecat_def.h"

#include "applInterface.h"

#include "rmcs_pd.h"

/* Called when an error state was acknowledged by the master. */
void APPL_AckErrorInd(UINT16 stateTrans) { (void)stateTrans; }

/* INIT -> PREOP: start the mailbox handler. */
UINT16 APPL_StartMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* PREOP -> INIT: stop the mailbox handler. */
UINT16 APPL_StopMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* PREOP -> SAFEOP: start the input handler. */
UINT16 APPL_StartInputHandler(UINT16 *pIntMask) {
    (void)pIntMask;
    return ALSTATUSCODE_NOERROR;
}

/* SAFEOP -> PREOP: stop the input handler. */
UINT16 APPL_StopInputHandler(void) { return ALSTATUSCODE_NOERROR; }

/* SAFEOP -> OP: outputs become valid -- the host link is (re)starting.
 * Reset the stream ARQ state; the master resets its own endpoint before
 * requesting OP, so both sides restart from seq/ack 0. */
UINT16 APPL_StartOutputHandler(void) {
    rmcs_pd_reset();
    return ALSTATUSCODE_NOERROR;
}

/* OP -> SAFEOP: outputs are no longer written by the master. Nothing to do:
 * the ARQ endpoint simply stops advancing until the next OP entry. */
UINT16 APPL_StopOutputHandler(void) { return ALSTATUSCODE_NOERROR; }

/* Process data sizes. Fixed-size stream chunks in both directions; MUST
 * match the byte-array PDOs configured in the SSC Tool project and the ESI
 * file (see ../README.md). */
UINT16 APPL_GenerateMapping(UINT16 *pInputSize, UINT16 *pOutputSize) {
    *pInputSize = RMCS_PD_CHUNK_SIZE;
    *pOutputSize = RMCS_PD_CHUNK_SIZE;
    return ALSTATUSCODE_NOERROR;
}

/* Copy the input stream chunk (slave -> master) into the ESC image. */
void APPL_InputMapping(UINT16 *pData) { rmcs_pd_build_inputs((uint8_t *)pData); }

/* Consume the output stream chunk (master -> slave) from the ESC image. */
void APPL_OutputMapping(UINT16 *pData) { rmcs_pd_on_outputs((const uint8_t *)pData); }

/* Cyclic application hook: unused, the stream is fully handled by the two
 * mapping callbacks above. */
void APPL_Application(void) {}

#if defined(EXPLICIT_DEVICE_ID) && EXPLICIT_DEVICE_ID
UINT16 APPL_GetDeviceID(void) { return 0x5; }
#endif
