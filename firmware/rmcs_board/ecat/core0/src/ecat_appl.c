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
#include "usb_runtime.h"

/* Cross-core uplink doorbell transport (see rmcs_uplink_doorbell_init). */
#include "board.h"
#include "hpm_clock_drv.h"
#include "hpm_interrupt.h"
#include "hpm_mbx_drv.h"

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

#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD
/* Hybrid fixed-PDO variant: 352 bytes = 28 x 12-byte cyclic CAN slots + a
 * 16-byte pd_stream chunk. Compile-check every size relationship so a mismatch
 * between the object dictionary, region split and SSC buffers cannot slip
 * through. */
_Static_assert(RMCS_PD_CHUNK_SIZE == 352, "hybrid PD is 352 bytes per direction");
_Static_assert(RMCS_STREAM_ENTRY_COUNT == 88, "hybrid maps 84 fixed + 4 stream UNSIGNED32");
_Static_assert(
    336 /* fixed region */ + 16 /* stream chunk */ == RMCS_PD_CHUNK_SIZE,
    "hybrid region offsets must partition the 352-byte PDO");
#endif

/* Called when an error state was acknowledged by the master. */
void APPL_AckErrorInd(UINT16 stateTrans) { (void)stateTrans; }

/* INIT -> PREOP: start the mailbox handler. */
UINT16 APPL_StartMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* PREOP -> INIT: stop the mailbox handler. */
UINT16 APPL_StopMailboxHandler(void) { return ALSTATUSCODE_NOERROR; }

/* Cross-core uplink doorbell (see rmcs_uplink_doorbell_init). The fieldbus
 * core pokes HPM_MBX0B right after pushing a reply into the up ring; core0
 * takes the matching HPM_MBX0A interrupt here. Priority stays strictly BELOW
 * the ESC PDI interrupt (4) so the doorbell can never preempt an in-flight
 * EtherCAT frame; the reverse preemption is fenced by DISABLE_ESC_INT inside
 * rmcs_input_refresh, so the two PDO_InputMapping sites never interleave. */
#define RMCS_UPLINK_MBX          HPM_MBX0A
#define RMCS_UPLINK_MBX_IRQ      IRQn_MBX0A
#define RMCS_UPLINK_MBX_PRIORITY 1

/* Publish freshly staged uplink into the ESC input image. Shared by the
 * doorbell ISR (fast path) and the pAPPL_MainLoop fallback below.
 *
 * In SM-synchron mode the SSC maps inputs only inside the SM2-event ISR,
 * which runs BEFORE the fieldbus core has produced its reply to the chunk
 * consumed in that very ISR: the reply then sits in the cross-core ring
 * until something republishes it. Doing so as soon as it exists lets the
 * master's next frame pick it up -- one poll cycle less end-to-end latency
 * on every request/response exchange.
 *
 * The ESC interrupt is masked around the copy so the PDI ISR's own
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

/* Doorbell handler: publish the reply the instant the fieldbus core produced
 * it, so the master's very next frame carries it -- the event-driven
 * counterpart of the pAPPL_MainLoop poll, without that loop's mailbox/CoE
 * slow-path granularity gating the turnaround. */
SDK_DECLARE_EXT_ISR_M(RMCS_UPLINK_MBX_IRQ, rmcs_uplink_doorbell_isr)
void rmcs_uplink_doorbell_isr(void) {
    /* Drain the poke word to deassert RWMV and re-arm the interrupt; its value
     * carries no information (the up ring is the source of truth). */
    uint32_t doorbell = 0;
    (void)mbx_retrieve_message(RMCS_UPLINK_MBX, &doorbell);
    (void)doorbell;
    rmcs_input_refresh();
    /* Same argument, USB side: the doorbell says a reply just landed in the up
     * ring, and without this the reply would wait for the next main-loop pump
     * pass. rmcs_input_refresh() above is a two-load no-op while USB owns the
     * link (no master -> bEcatInputUpdateRunning is FALSE), so the two publish
     * paths do not fight; it stays unconditional to keep EtherCAT behaviour
     * bit-identical. The main-loop pump masks this IRQ (see
     * rmcs_uplink_doorbell_set_enabled), so only one pump runs at a time. */
    if (rmcs_pd_usb_active())
        rmcs_usb_runtime_pump();
}

/* Mask/unmask the doorbell around a thread-context vendor pump, so the pump in
 * the ISR above cannot preempt it and interleave on the vendor FIFOs / rings.
 * A doorbell arriving while masked stays pending and fires right after. */
void rmcs_uplink_doorbell_set_enabled(bool enabled) {
    if (enabled)
        intc_m_enable_irq(RMCS_UPLINK_MBX_IRQ);
    else
        intc_m_disable_irq(RMCS_UPLINK_MBX_IRQ);
}

/* MainLoop fallback around the same publish. The doorbell ISR handles the
 * common case; this still runs every SSC MainLoop pass to cover an uplink
 * that became publishable with no in-cycle event left to re-trigger it (e.g.
 * the doorbell arrived while the ARQ still had a chunk in flight). Mask the
 * doorbell IRQ so it cannot preempt this thread-context mapping and interleave
 * a second PDO_InputMapping. */
static void rmcs_input_refresh_mainloop(void) {
    intc_m_disable_irq(RMCS_UPLINK_MBX_IRQ);
    rmcs_input_refresh();
    intc_m_enable_irq(RMCS_UPLINK_MBX_IRQ);
}

/* Arm the cross-core uplink doorbell on core0. Enables the shared MBX0 clock
 * (which also powers HPM_MBX0B on the fieldbus core), resets the mailbox, and
 * unmasks the word-received interrupt. MUST run BEFORE core1 is released so
 * the clock is up before core1 first touches HPM_MBX0B. */
void rmcs_uplink_doorbell_init(void) {
    clock_add_to_group(clock_mbx0, 0);
    mbx_init(RMCS_UPLINK_MBX);
    mbx_enable_intr(RMCS_UPLINK_MBX, MBX_CR_RWMVIE_MASK);
    intc_m_enable_irq_with_priority(RMCS_UPLINK_MBX_IRQ, RMCS_UPLINK_MBX_PRIORITY);
}

/* PREOP -> SAFEOP: start the input handler. The non-const pointer signature
 * is the SSC contract (stacks may adjust the AL event mask through it). */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
UINT16 APPL_StartInputHandler(UINT16* pIntMask) {
    (void)pIntMask;
    pAPPL_MainLoop = rmcs_input_refresh_mainloop;
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
