/*
 * Core1 entry of the EtherCAT-on-core1 probe image (migration step 0).
 *
 * Evolved from ../../core0/src/ecat_main.c. What is kept: ESC bring-up,
 * ecat_hardware_init(), the internal-PHY MII/link handling, MainInit(),
 * APPL_GenerateMapping() and the MainLoop + PHY-poll loop. What is gone, because
 * it belongs to core0 in the new layout: rmcs_usb_runtime_*, rmcs_pd_init(),
 * rmcs_uplink_doorbell_init() and multicore_release_cpu() -- this image IS the
 * released core.
 *
 * Purpose (../../CORE_SWAP_MIGRATION.md section 4, step 0): retire risks 1
 * (core1 printf), 3 (clock-group ownership) and 10 (cross-core ESC access) in
 * one shot, with the flash-write delegation deliberately isolated out (the
 * emulated EEPROM is read-only here, see hpm_ecat_e2p_emulation.c).
 *
 * Pass criteria: the master enumerates the slave and reaches PREOP,
 * ecat_time_ms is advancing, and core0's USB and CAN loopbacks are unaffected.
 */

#include "applInterface.h"
#include "board.h"
#include "digital_io.h" /* APPL_* prototypes (implemented in ecat_appl.c) */
#include "ecat_def.h"
#include "ecatappl.h"
#include "ecatslv.h"
#include "hpm_ecat_hw.h"

#include "ecat_diag.h"
#include "ecat_doorbell.h"
#if defined(RMCS_SSC_HAS_FOE) && RMCS_SSC_HAS_FOE
# include "ecat_foe_support.h"
#endif
#include "ecat_pd.h"
#include "ecat_xcore.h"

#define RMCS_ECAT_PHY_LINK_POLL_INTERVAL_MS (100U)
/* Heartbeat cadence for the diagnostic ring. Long enough that the log is not the
 * thing being measured, short enough that a stalled MainLoop is obvious. */
#define RMCS_ECAT_HEARTBEAT_INTERVAL_MS (1000U)

int main(void) {
    hpm_stat_t stat;

    /* Includes board_init_pmp(): maps SHARE_RAM non-cacheable with AMO, which the
     * cross-core rings require. Deliberately does NOT init a console -- UART1 (the
     * SDK's core1 console) is this board's fieldbus data port. */
    board_init_core1();

    /* Claim the channel before anything that can log: until this returns, every
     * ecat_diag_printf() is dropped on the floor. It blocks on the magic word, so
     * it also serialises this core behind core0's publication. */
    if (!ecat_xcore_init()) {
        /* Mismatched core0/core1 image pair. Stopping here is the point of the
         * version field: touching rings whose layout changed is worse than a
         * silent core. Nothing can report this -- the diagnostic ring is part of
         * the layout in question -- so the observable symptom is "core1 never
         * logs", and the fix is to rebuild both images together. */
        while (1) {}
    }

    printf("RMCS EtherCAT probe (core1), channel version %u\n", ecat_xcore_channel_version());

    /* Bind the process-data rings BEFORE MainInit(): from that point on the SSC
     * may invoke the PDO hooks, and they dereference the channel unconditionally
     * (an unbound endpoint is a null dereference, not a benign no-op). */
    ecat_pd_init();

    /* Arm the uplink doorbell after the rings are bound (the handler touches
     * them) and before MainInit(), so no publish opportunity is missed once the
     * SSC starts running. */
    ecat_doorbell_init();

    /* Clocks the ESC and muxes its pins. clock_esc0 lands in whichever group
     * board.c assigns; per section 2.1 the ESC group is bound to core0, which is
     * permanently running in the new layout, so the peripheral stays enabled and
     * this core can reach it. That is precisely what this probe verifies. */
    board_init_ethercat(HPM_ESC);

    stat = ecat_hardware_init(HPM_ESC);

    /* Every erase and program this image will ever ask core0 for happens inside
     * ecat_hardware_init() -> ecat_flash_eeprom_init(), whose boot window closes
     * before it returns. Releasing core0 here -- on the failure path as well, or
     * it would wait out its whole timeout for a rewrite that is not coming --
     * is what keeps a first-boot SII rewrite from stalling an enumerated USB
     * host or overrunning a live MCAN receiver. */
    ecat_xcore_signal_eeprom_ready();

    if (stat != status_success) {
        printf("Init ESC peripheral and related devices (EEPROM/PHY) failed!\n");
        return 0;
    }

    /* The on-die PHYs do not route a stable LINK pin to an ESC CTR input, so the
     * SDK's NMII_LINK-from-IO path is not usable on this board. Drive the ESC link
     * state from the real PHY BMSR instead: forcing both ports up breaks the
     * single-cable case because the empty port never closes its loop. */
    board_ecat_configure_internal_phy_mii_mode();
    board_ecat_wait_internal_phy_link(2000);

    MainInit(); /* SSC stack initialization */

#if defined(ESC_EEPROM_EMULATION) && ESC_EEPROM_EMULATION
    pAPPL_EEPROM_Read = ecat_eeprom_emulation_read;
    /* Still installed even though the project copy refuses to write: the SSC
     * needs a non-NULL handler to answer an EEPROM write command at all, and the
     * refusal is what turns into ESC_EEPROM_EMULATION_ACK_ERROR on the wire. */
    pAPPL_EEPROM_Write = ecat_eeprom_emulation_write;
    pAPPL_EEPROM_Reload = ecat_eeprom_emulation_reload;
    pAPPL_EEPROM_Store = ecat_eeprom_emulation_store;
#endif

#if defined(RMCS_SSC_HAS_FOE) && RMCS_SSC_HAS_FOE
    /* After MainInit(): it resets the pAPPL_* pointer set this installs into. */
    (void)ecat_foe_support_init();
#endif

    APPL_GenerateMapping(&nPdInputSize, &nPdOutputSize);

    printf("SSC up, pd in/out %u/%u bytes\n", nPdInputSize, nPdOutputSize);

    bRunApplication = TRUE;
    uint32_t last_phy_link_poll_ms = HW_GetTimer();
    uint32_t last_heartbeat_ms = last_phy_link_poll_ms;
    while (bRunApplication == TRUE) {
        MainLoop();

#if defined(RMCS_SSC_HAS_FOE) && RMCS_SSC_HAS_FOE
        /* Never returns once a staged download is pending: it cold-resets so the
         * bootloader can install. Placed after MainLoop() so the FoE response
         * that completed the transfer has already gone out. */
        ecat_foe_poll_reset();
#endif

        const uint32_t now_ms = HW_GetTimer();

        /* The internal PHY link signals reach the ESC through software-driven
         * GPR bits, not dedicated LINK pins. Refresh them throughout runtime so
         * cable insertion after boot and transient link loss recover without a
         * power cycle. The PDI IRQ preempts this thread-context MDIO poll. */
        if ((uint32_t)(now_ms - last_phy_link_poll_ms)
            >= RMCS_ECAT_PHY_LINK_POLL_INTERVAL_MS) {
            last_phy_link_poll_ms = now_ms;
            (void)board_ecat_refresh_internal_phy_link();
        }

        /* Publishes the two step-0 pass criteria that are otherwise invisible
         * from the master side: HW_GetTimer() advancing proves the GPTMR0 clock
         * group reached this core, and nAlStatus tracks the AL state machine. */
        if ((uint32_t)(now_ms - last_heartbeat_ms) >= RMCS_ECAT_HEARTBEAT_INTERVAL_MS) {
            last_heartbeat_ms = now_ms;
            /* Raw ESC registers alongside the SSC's software copy. The RUN LED
             * is generated by ESC HARDWARE from AL_STATUS (0x0130), not from
             * nAlStatus, so a blinking LED with nAlStatus reading INIT can only
             * be explained by looking at the register itself. DL_STATUS (0x0110)
             * carries the per-port link bits the ESC actually sees. */
            uint16_t al_control = 0, al_status_reg = 0, dl_status = 0;
            HW_EscReadWord(al_control, ESC_AL_CONTROL_OFFSET);
            HW_EscReadWord(al_status_reg, ESC_AL_STATUS_OFFSET);
            HW_EscReadWord(dl_status, 0x0110); /* DL Status; esc.h has no macro for it */
            printf(
                "core1 alive: t=%u ms, nAlStatus=0x%x | ESC al_ctl=0x%04x al_sts=0x%04x "
                "dl_sts=0x%04x\n",
                now_ms, nAlStatus, al_control, al_status_reg, dl_status);
        }
    }

    return 0;
}
