/*
 * SSC application layer for the EtherCAT core1 probe image.
 *
 * Derived from ../../core0/src/ecat_appl.c with the process-data body removed:
 * the full callback set the SSC requires is present with unchanged signatures,
 * but the two mapping hooks are minimal stubs -- outputs discarded, inputs
 * zero-filled. The ARQ endpoint and the cross-core rings arrive in step 1 of
 * ../../CORE_SWAP_MIGRATION.md; step 0 only has to prove the master can enumerate
 * the slave and reach PREOP with the stack running on core1.
 *
 * Consequences to expect on the bench, so they are not mistaken for faults: no
 * cross-core traffic, and the input image reads as zeros in SAFEOP/OP. There is
 * also no uplink doorbell here -- core0 owns the mailbox reversal (step 2).
 *
 * Stays pure C so it only depends on the Beckhoff SSC headers being C-compilable
 * (which they are by construction).
 */

#include <stdint.h>
#include <string.h>

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

/* The override header defines this marker; a stock generated header would
 * declare 4-byte counter PDOs that silently disagree with the stream chunk. */
#ifndef RMCS_STREAM_OBJECTS
# error "Stock digital_ioObjects.h detected; run ecat/tools/import_ssc.sh"
#endif

/* RMCS_PD_CHUNK_SIZE comes from ecat_pd.h, which also static_asserts it against
 * librmcs::ecat::kPdChunkSize on the C++ side. The asserts below close the loop
 * from the other direction: object dictionary mapping and SSC buffer sizes. */
#include "ecat_doorbell.h"
#include "ecat_pd.h"

#if defined(RMCS_SSC_HAS_FOE) && RMCS_SSC_HAS_FOE
# include "ecat_foe_support.h"
#endif

/* Uplink doorbell transport. core0 pokes MBX0A, this core takes MBX0B.
 *
 * Priority 1 is strictly below the ESC PDI interrupt (4, set by the SDK port
 * layer) so the doorbell can never preempt an in-flight EtherCAT frame. */
#include <hpm_interrupt.h>
#include <hpm_mbx_drv.h>
#define RMCS_DOORBELL_MBX      HPM_MBX0B
#define RMCS_DOORBELL_IRQ      IRQn_MBX0B
#define RMCS_DOORBELL_PRIORITY 1

_Static_assert(
    RMCS_STREAM_ENTRY_COUNT * 4 == RMCS_PD_CHUNK_SIZE,
    "object dictionary PDO mapping and stream chunk size must agree");
_Static_assert(
    MAX_PD_INPUT_SIZE >= RMCS_PD_CHUNK_SIZE && MAX_PD_OUTPUT_SIZE >= RMCS_PD_CHUNK_SIZE,
    "SSC process data buffers too small; check core1_ecat/CMakeLists.txt definitions");

/* Called when an error state was acknowledged by the master. */
void APPL_AckErrorInd(UINT16 stateTrans) { (void)stateTrans; }

/* INIT -> PREOP: start the mailbox handler. */
UINT16 APPL_StartMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* PREOP -> INIT: stop the mailbox handler.
 *
 * Also the point where a completed FoE download turns into a reboot. The master
 * reaches here by leaving BOOT, which means the transfer is finished on the wire
 * and its last response has been sent -- resetting any earlier aborts a transfer
 * that actually succeeded, which was measured: the master reported
 * FOE_RECEIVE_ERROR for a download whose every byte had been staged. */
UINT16 APPL_StopMailboxHandler(void) {
#if defined(RMCS_SSC_HAS_FOE) && RMCS_SSC_HAS_FOE
    if (ecat_foe_download_complete())
        ecat_foe_request_reset();
#endif
    return ALSTATUSCODE_NOERROR;
}

/* Republish freshly staged uplink into the ESC input image.
 *
 * Why this is needed at all: in SM-synchron mode the SSC maps inputs only inside
 * the SM2-event ISR, which runs BEFORE core0 has produced its reply to the chunk
 * consumed in that very ISR. The reply then sits in the up ring until something
 * republishes it -- costing a full poll cycle on every request/response
 * exchange. Doing it as soon as the reply exists lets the master's next frame
 * pick it up.
 *
 * The ESC interrupt is masked around the copy so the PDI ISR's own
 * PDO_InputMapping cannot interleave; the 3-buffer SyncManager swaps atomically,
 * so the extra write is invisible to a mid-read master. The pending check keeps
 * the fast path to two shared-memory loads and bounds the extra ESC traffic to
 * one write per acknowledged chunk.
 *
 * Step 1 drives this from the SSC main loop only. Step 2 adds the cross-core
 * doorbell (core0 pokes MBX0A when it pushes a batch) so the turnaround drops
 * from main-loop granularity -- which mixes in mailbox/CoE slow paths, tens of
 * microseconds of jitter -- to interrupt latency. This hook then stays as the
 * fallback for an uplink that became publishable with no in-cycle event left to
 * re-trigger it. */
static void rmcs_input_refresh(void) {
    if (bEcatInputUpdateRunning && ecat_pd_uplink_pending()) {
        DISABLE_ESC_INT();
        PDO_InputMapping();
        ENABLE_ESC_INT();
    }
}

/* Uplink doorbell handler: core0 pushed a batch into the up ring and poked us,
 * so publish it now instead of waiting for the next MainLoop pass. See
 * ecat_doorbell.h for the direction change and the priority argument. */
SDK_DECLARE_EXT_ISR_M(RMCS_DOORBELL_IRQ, rmcs_uplink_doorbell_isr)
void rmcs_uplink_doorbell_isr(void) {
    /* Drain the poke word to deassert RWMV and re-arm the interrupt; its value
     * carries no information (the up ring is the source of truth). */
    uint32_t doorbell = 0;
    (void)mbx_retrieve_message(RMCS_DOORBELL_MBX, &doorbell);
    (void)doorbell;
    rmcs_input_refresh();
}

void ecat_doorbell_init(void) {
    mbx_init(RMCS_DOORBELL_MBX);
    mbx_enable_intr(RMCS_DOORBELL_MBX, MBX_CR_RWMVIE_MASK);
    intc_m_enable_irq_with_priority(RMCS_DOORBELL_IRQ, RMCS_DOORBELL_PRIORITY);
}

/* PREOP -> SAFEOP: start the input handler. The non-const pointer signature is
 * the SSC contract (stacks may adjust the AL event mask through it). */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
UINT16 APPL_StartInputHandler(UINT16* pIntMask) {
    (void)pIntMask;
    pAPPL_MainLoop = rmcs_input_refresh;
    return ALSTATUSCODE_NOERROR;
}

/* SAFEOP -> PREOP: stop the input handler. Drop the republish hook with it: the
 * ESC input image is no longer being read, and PDO_InputMapping outside the
 * SAFEOP/OP window is not meaningful. */
UINT16 APPL_StopInputHandler(void) {
    pAPPL_MainLoop = NULL;
    return ALSTATUSCODE_NOERROR;
}

/* SAFEOP -> OP: outputs become valid. Reset the ARQ endpoint, drain the ring we
 * consume, and claim the data plane from core0 (see ecat_pd_reset). */
UINT16 APPL_StartOutputHandler(void) {
    ecat_pd_reset();
    return ALSTATUSCODE_NOERROR;
}

/* OP -> SAFEOP: outputs are no longer written by the master. */
UINT16 APPL_StopOutputHandler(void) { return ALSTATUSCODE_NOERROR; }

/* Process data sizes. Fixed-size stream chunks in both directions; MUST match
 * the byte-array PDOs configured in the SSC Tool project and the ESI file (see
 * ../../README.md). Unchanged from core0 -- the master-visible interface must
 * stay identical across the migration, or the probe would be testing a different
 * slave. */
UINT16 APPL_GenerateMapping(UINT16* pInputSize, UINT16* pOutputSize) {
    *pInputSize = RMCS_PD_CHUNK_SIZE;
    *pOutputSize = RMCS_PD_CHUNK_SIZE;
    return ALSTATUSCODE_NOERROR;
}

/* Copy the input stream chunk (slave -> master) into the ESC image: the ARQ
 * endpoint stages an uplink chunk, pulling payload bytes from the up ring that
 * core0 fills. */
void APPL_InputMapping(UINT16* pData) { ecat_pd_build_inputs((uint8_t*)pData); }

/* Consume the output stream chunk (master -> slave) from the ESC image and push
 * accepted payload into the down ring for core0's deserializer. */
void APPL_OutputMapping(UINT16* pData) { ecat_pd_on_outputs((const uint8_t*)pData); }

/* Cyclic application hook: unused, as on core0. */
void APPL_Application(void) {}

#if defined(EXPLICIT_DEVICE_ID) && EXPLICIT_DEVICE_ID
UINT16 APPL_GetDeviceID(void) { return 0x5; }
#endif
