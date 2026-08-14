#ifndef ECAT_FOE_SUPPORT_H
#define ECAT_FOE_SUPPORT_H

/* Board side of the SDK's FoE port layer.
 *
 * hpm_ecat_foe.h includes THIS file by name, so it has to exist under a
 * directory on the include path and define the three FOE_FILE_* macros the
 * generic glue uses. The SDK ships its own copy next to the ecat_foe sample; we
 * deliberately do not use it, because it points FoE at an 8 KiB scratch area and
 * programs the flash inline -- neither of which is possible here:
 *
 *   * the destination is the FoE staging region, not a scratch file, because the
 *     received bytes are a firmware image the bootloader will install;
 *   * this core is a pure RAM image with no access to the NOR at all, so every
 *     erase and program crosses to core0 through the MBX1 flash RPC.
 *
 * The addresses below mirror common/foe_staging.hpp. They are spelled out in C
 * arithmetic because this header is included from C, while foe_staging.hpp is
 * C++; ecat_foe_support.cpp static_asserts the two against each other so the
 * duplication cannot drift silently.
 */

#include "board.h"
#include "ecat_def.h"

/* Image area: the staging region minus its leading metadata sector. */
#define FOE_FILE_ADDR                                                                              \
    (BOARD_FLASH_BASE_ADDRESS + BOARD_FOE_STAGING_ADDR + BOARD_FOE_STAGING_METADATA_SIZE)

/* Bounded by the APP SLOT, not by the staging region -- see foe_staging.hpp.
 * 0x20000 is the bootloader + app-metadata reservation below the app slot. */
#define FOE_FILE_MAX_SIZE (BOARD_APP_FLASH_END_OFFSET - 0x20000UL)

/* Only used by the port layer's cache-invalidate range on the read path. */
#define FOE_FILE_TOTAL_SIZE (FOE_FILE_ADDR + FOE_FILE_MAX_SIZE)

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the four board hooks the port layer calls, and the four pAPPL_Foe*
 * the SSC calls. Call AFTER MainInit(), like the SDK sample does: the pAPPL_*
 * pointers live in the stack and MainInit() resets them.
 *
 * Returns true on success. Failure means the flash RPC is unavailable, in which
 * case FoE stays unhooked and the slave answers FoE requests with an error --
 * which is the right outcome: a slave that accepts a firmware download it cannot
 * store is worse than one that refuses. */
bool ecat_foe_support_init(void);

/* True once a download has been staged and committed. */
bool ecat_foe_download_complete(void);

/* Called from APPL_StopMailboxHandler (PREOP -> INIT) once a download has been
 * staged: the master has left BOOT, so the transfer is finished on the wire --
 * including the acknowledgement of its final block -- and the board may reboot
 * to let the bootloader install. Resetting any earlier loses that ack and the
 * master reports a failure for a transfer that fully succeeded. */
void ecat_foe_request_reset(void);

/* Pumped from the main loop; performs the reset the call above requested. */
void ecat_foe_poll_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SUPPORT_H */
