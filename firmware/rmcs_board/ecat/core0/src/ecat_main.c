/*
 * Core0 entry of the RMCS EtherCAT stream bridge.
 *
 * Structure follows the SDK's ecat_io sample (samples/ethercat/ecat_io/
 * ecat.c) plus the dual-core bring-up from samples/multicore/hello: core0
 * owns the ESC and the Beckhoff SSC stack; core1 (the fieldbus core) is
 * loaded into its own ILM and released after the shared-memory channel is
 * published.
 */

#include <stdio.h>

#include "applInterface.h"
#include "board.h"
#include "digital_io.h" /* APPL_* prototypes (implemented in ecat_appl.c) */
#include "ecat_def.h"
#include "ecatappl.h"
#include "ecatslv.h"
#include "hpm_ecat_hw.h"
#include "multicore_common.h"
#include "rmcs_pd.h"

int main(void) {
    hpm_stat_t stat;

    board_init();
    board_init_ethercat(HPM_ESC);
    printf("RMCS EtherCAT stream bridge (core0)\n");

    /* Publish the shared-memory channel and arm the cross-core uplink doorbell,
     * then start the fieldbus core. Order matters: core1 spins on the channel
     * magic (stored by rmcs_pd_init) and poll-free uplink relies on the MBX0
     * clock being up (enabled by rmcs_uplink_doorbell_init) before core1 runs. */
    rmcs_pd_init();
    rmcs_uplink_doorbell_init();
    multicore_release_cpu(HPM_CORE1, SEC_CORE_IMG_START);

    stat = ecat_hardware_init(HPM_ESC);
    if (stat != status_success) {
        printf("Init ESC peripheral and related devices (EEPROM/PHY) failed!\n");
        return 0;
    }

    MainInit(); /* SSC stack initialization */

#if defined(ESC_EEPROM_EMULATION) && ESC_EEPROM_EMULATION
    pAPPL_EEPROM_Read = ecat_eeprom_emulation_read;
    pAPPL_EEPROM_Write = ecat_eeprom_emulation_write;
    pAPPL_EEPROM_Reload = ecat_eeprom_emulation_reload;
    pAPPL_EEPROM_Store = ecat_eeprom_emulation_store;
#endif

    APPL_GenerateMapping(&nPdInputSize, &nPdOutputSize);

    bRunApplication = TRUE;
    while (bRunApplication == TRUE) {
        MainLoop();
    }

    return 0;
}
