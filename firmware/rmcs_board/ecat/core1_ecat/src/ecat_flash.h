#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_FLASH_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "hpm_common.h"

/*
 * core1 side of the flash-write delegation (../../CORE_SWAP_MIGRATION.md
 * section 3.2).
 *
 * WHY THIS EXISTS. There is one NOR on this part (XPI0) and the emulated SII
 * EEPROM lives in it. A NOR cannot be erased/programmed and fetched from at the
 * same time, and in the swapped layout core0 is the XIP core. core1 is a pure
 * RAM image and is immune to the stall, but it must not drive the controller
 * core0 executes from -- so the two MUTATING flash operations are shipped to
 * core0 over SHARE_RAM and core0 runs the ROM API with interrupts masked.
 *
 * WHAT IS *NOT* HERE, deliberately: the read path. nor_flash_read() is a memcpy
 * from the XIP window plus a cache invalidate, it never touches the ROM API, and
 * SII upload is a hot path -- it stays local to core1 (see
 * hpm_ecat_e2p_emulation.c). Only e2p_config_t::flash_write and
 * ::flash_erase become cross-core stubs; the whole e2p state machine, including
 * its 32 KiB RAM index table, stays on core1. Splitting the state machine
 * instead ("core1 reads, core0 writes") would leave core1's index table stale
 * after every core0 write, which is why that shape was rejected.
 *
 * GRANULARITY. One RPC == one ROM API call: one program of at most
 * kXcoreFlashPayloadSize bytes, or ONE sector erase. Multi-sector erases are
 * split by ecat_flash_rpc_erase() into one round trip per sector, so core0 gets
 * its interrupts back between sectors instead of staying masked for the whole
 * 32 KiB area. That bounds the worst-case core0 interrupt-off window to a single
 * sector erase (tens of ms) rather than eight of them.
 *
 * BLOCKING. The caller busy-waits for core0. That is safe because every write
 * path into e2p originates in EEPROM_CommandHandler(), which ECAT_Main() calls
 * from THREAD context, not from the PDI ISR. Note that eeprom_emulation.c wraps
 * each flash op in E2P_CRITICAL_ENTER/EXIT (= disable_global_irq on core1), so
 * the wait does run with core1's interrupts masked; the boot-window policy below
 * is what keeps that off the data plane.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the RPC block published by core0 and cross-check its geometry against
 * this image's ecat_config.h. Returns false when core0 published no usable
 * flash server (rom_xpi_nor_auto_config failed there, or the core0 image
 * predates this feature) or when the two sides disagree about the emulated
 * EEPROM window -- in both cases the caller must keep the EEPROM read-only
 * rather than issue writes into an unvalidated address range.
 *
 * Must run after ecat_xcore_init(): it reads the channel. */
bool ecat_flash_rpc_init(uint32_t window_start, uint32_t window_size, uint32_t sector_size);

/* The boot window. ERASES ARE ACCEPTED ONLY WHILE IT IS OPEN.
 *
 * ../../CORE_SWAP_MIGRATION.md section 3.2 forbids running the e2p garbage
 * collector on the data plane, and an erase is the only unbounded-cost
 * operation here (a whole sector, tens of ms of core0 interrupt-off; USB NAKs
 * and MCAN RX overruns follow). Gating at this level rather than at the call
 * sites means EVERY path that could erase -- e2p_config()'s version-mismatch
 * format, e2p_write()'s implicit E2P_FLUSH_FORCE, e2p_clear() -- is covered by
 * construction, including paths added by a future SDK update.
 *
 * The window is opened and closed inside ecat_flash_eeprom_init(), which runs
 * from ecat_hardware_init() before MainInit(): the SSC cannot be running and no
 * master can be issuing commands yet. */
void ecat_flash_open_boot_window(void);
void ecat_flash_close_boot_window(void);
bool ecat_flash_boot_window_open(void);

/* e2p_config_t::flash_write. Split into kXcoreFlashPayloadSize-sized round
 * trips; `addr` is an absolute address in the XIP window, as e2p uses. */
hpm_stat_t ecat_flash_rpc_program(const uint8_t* buf, uint32_t addr, uint32_t size);

/* e2p_config_t::flash_erase. `size` must be a whole number of sectors. Refused
 * outside the boot window (see above). */
hpm_stat_t ecat_flash_rpc_erase(uint32_t start_addr, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_FLASH_H */
