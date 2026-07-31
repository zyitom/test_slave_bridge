#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/* Configuration for the SDK flash-EEPROM-emulation component, consumed by
 * samples/ethercat/port/hpm_ecat_e2p_emulation.h. Values follow
 * ../../core0/src/user_config.h. */

#include "ecat_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NONE, not WARN as on core0. This compiles out all 62 e2p_err/e2p_warn/e2p_info
 * call sites in components/eeprom_emulation/eeprom_emulation.c, which is the
 * first of the three printf countermeasures required by
 * ../../CORE_SWAP_MIGRATION.md section 3.3. The other two -- the printf redirect
 * and the strong _write() -- would catch these too, but compiling them out is
 * what actually removes the code and, with it, any chance of a log line firing
 * from inside a flash critical section. */
#define E2P_DEBUG_LEVEL E2P_DEBUG_LEVEL_NONE
#define E2P_MAX_VAR_CNT (ESC_EEPROM_SIZE)

#ifdef __cplusplus
}
#endif

#endif /* USER_CONFIG_H */
