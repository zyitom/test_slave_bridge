/*
 * SSC application layer for the RMCS EtherCAT stream bridge.
 *
 * Mirrors the callback set of the SDK's ecat_io sample (application/
 * digital_io.c) but maps a fixed-size byte-stream chunk instead of digital
 * IO. All real work happens behind the C boundary in rmcs_pd.h; this file
 * stays pure C so it only depends on the Beckhoff SSC headers being
 * C-compilable (which they are by construction).
 */

#include <stdint.h>

#include "applInterface.h"
#include "ecat_def.h"

/* Instantiate the application object dictionary (ApplicationObjDic variables)
 * exactly once, in this translation unit. digital_io.h is the generated
 * application header whose object content is replaced at import time by
 * ssc_overrides/digital_ioObjects.h (see tools/import_ssc.sh); the generated
 * digital_io.c application template is not compiled. */
#define _DIGITAL_IO_ 1
#include "digital_io.h"
#undef _DIGITAL_IO_

#include "rmcs_pd.h"

/* The override header defines this marker; a stock generated header would
 * declare 4-byte counter PDOs that silently disagree with the stream chunk. */
#ifndef RMCS_STREAM_OBJECTS
# error "Stock digital_ioObjects.h detected; run ecat/tools/import_ssc.sh"
#endif

_Static_assert(
    RMCS_STREAM_ENTRY_COUNT * 4 == RMCS_PD_CHUNK_SIZE,
    "object dictionary PDO mapping and stream chunk size must agree");
_Static_assert(
    MAX_PD_INPUT_SIZE >= RMCS_PD_CHUNK_SIZE && MAX_PD_OUTPUT_SIZE >= RMCS_PD_CHUNK_SIZE,
    "SSC process data buffers too small; check core0/CMakeLists.txt definitions");

/* Called when an error state was acknowledged by the master. */
void APPL_AckErrorInd(UINT16 stateTrans) { (void)stateTrans; }

/* INIT -> PREOP: start the mailbox handler. */
UINT16 APPL_StartMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* PREOP -> INIT: stop the mailbox handler. */
UINT16 APPL_StopMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* Out-of-cycle input refresh, run from the SSC main loop via pAPPL_MainLoop.
 *
 * In SM-synchron mode the SSC maps inputs only inside the SM2-event ISR,
 * which runs BEFORE the fieldbus core has produced its reply to the chunk
 * consumed in that very ISR: the reply then sits in the cross-core ring for
 * a full master poll cycle. Publishing it from the main loop as soon as it
 * exists lets the master's next frame pick it up -- one poll cycle less
 * end-to-end latency on every request/response exchange.
 *
 * The ESC interrupt is masked around the copy so the ISR's own
 * PDO_InputMapping cannot interleave; the 3-buffer SyncManager swaps
 * atomically, so the extra write is invisible to a mid-read master. The
 * pending check keeps the fast path to two shared-memory loads and bounds
 * the extra ESC traffic to one write per acknowledged chunk. */
static void rmcs_input_refresh(void) {
    if (bEcatInputUpdateRunning && rmcs_pd_uplink_pending()) {
        DISABLE_ESC_INT();
        PDO_InputMapping();
        ENABLE_ESC_INT();
    }
}

/* PREOP -> SAFEOP: start the input handler. The non-const pointer signature
 * is the SSC contract (stacks may adjust the AL event mask through it). */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
UINT16 APPL_StartInputHandler(UINT16* pIntMask) {
    (void)pIntMask;
    pAPPL_MainLoop = rmcs_input_refresh;
    return ALSTATUSCODE_NOERROR;
}

/* SAFEOP -> PREOP: stop the input handler. */
UINT16 APPL_StopInputHandler(void) {
    pAPPL_MainLoop = NULL;
    return ALSTATUSCODE_NOERROR;
}

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
UINT16 APPL_GenerateMapping(UINT16* pInputSize, UINT16* pOutputSize) {
    *pInputSize = RMCS_PD_CHUNK_SIZE;
    *pOutputSize = RMCS_PD_CHUNK_SIZE;
    return ALSTATUSCODE_NOERROR;
}

/* Copy the input stream chunk (slave -> master) into the ESC image. */
void APPL_InputMapping(UINT16* pData) { rmcs_pd_build_inputs((uint8_t*)pData); }

/* Consume the output stream chunk (master -> slave) from the ESC image. */
void APPL_OutputMapping(UINT16* pData) { rmcs_pd_on_outputs((const uint8_t*)pData); }

/* Cyclic application hook: unused, the stream is fully handled by the two
 * mapping callbacks above. */
void APPL_Application(void) {}

#if defined(EXPLICIT_DEVICE_ID) && EXPLICIT_DEVICE_ID
UINT16 APPL_GetDeviceID(void) { return 0x5; }
#endif
