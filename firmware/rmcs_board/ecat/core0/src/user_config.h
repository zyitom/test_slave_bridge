#ifndef _USER_CONFIG_H
#define _USER_CONFIG_H

/* Configuration for the SDK flash-EEPROM-emulation component, consumed by
 * samples/ethercat/port/hpm_ecat_e2p_emulation.h. Values follow the ecat_io
 * sample. */

#include "ecat_def.h"

#ifdef __cplusplus
extern "C" {
#endif

#define E2P_DEBUG_LEVEL E2P_DEBUG_LEVEL_WARN /* INFO level printf-spams the console */
#define E2P_MAX_VAR_CNT (ESC_EEPROM_SIZE)

#ifdef __cplusplus
}
#endif

#endif /* _USER_CONFIG_H */
