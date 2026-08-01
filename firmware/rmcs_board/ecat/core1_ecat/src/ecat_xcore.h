#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_XCORE_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_XCORE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * C boundary onto the cross-core channel (../../common/xcore_channel.hpp),
 * mirroring what rmcs_pd.h does for core0. The SSC-facing translation units of
 * this image stay pure C so they carry no dependency on the SSC headers being
 * C++-clean; ecat_xcore.cpp is the single unit that touches SHARE_RAM.
 *
 * Step 0 of the core swap (../../CORE_SWAP_MIGRATION.md section 4) deliberately
 * stops here: the channel is claimed and the diagnostic ring is wired up, but
 * the ARQ endpoint and the process-data rings are NOT connected yet. That is
 * step 1.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Block until core0 publishes the channel (xcore_channel_wait), then bind the
 * diagnostic ring so logging works. Must be the first thing main() does after
 * board_init_core1(): everything below it may log.
 *
 * Returns false when core0 published a channel whose version this image does
 * not implement -- a mismatched core0/core1 image pair. The caller must treat
 * that as fatal; touching the rings across a layout change is worse than
 * stopping. */
bool ecat_xcore_init(void);

/* Observed channel version, for the boot banner. Zero before ecat_xcore_init. */
uint32_t ecat_xcore_channel_version(void);

/* Announce that the boot-path EEPROM work is finished, so core0 may bring USB
 * and the CAN controllers up. Call exactly once, right after
 * ecat_hardware_init() returns -- on the failure path too, otherwise core0
 * waits out its whole timeout for a rewrite that is never coming. See
 * XcoreFlashRpc::eeprom_ready in ../../common/xcore_channel.hpp. */
void ecat_xcore_signal_eeprom_ready(void);

#ifdef __cplusplus
/* The bound channel, for the process-data plane (ecat_pd.cpp). C++ only: the
 * pure-C translation units here have no XcoreChannel declaration and must go
 * through the C wrappers in ecat_pd.h instead. Null until ecat_xcore_init()
 * succeeds. */
} /* extern "C" */
namespace librmcs::firmware::ecat {
struct XcoreChannel;
}
librmcs::firmware::ecat::XcoreChannel* ecat_xcore_channel(void);
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_XCORE_H */
