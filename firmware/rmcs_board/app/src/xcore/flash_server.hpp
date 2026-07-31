#pragma once

namespace librmcs::firmware::xcore {

// Core0 side of the flash-write delegation (ecat/CORE_SWAP_MIGRATION.md
// section 3.2, migration step 3).
//
// There is one NOR on this part (XPI0) and the EtherCAT stack's emulated SII
// EEPROM lives in it. core1 owns the whole EEPROM-emulation state machine --
// including its 32 KiB RAM index table, which is exactly why the state machine
// cannot be split across the cores -- but it must not drive the XPI0 controller
// this core executes from. So core1 keeps the read path (a memcpy from the XIP
// window, no ROM API) and ships programs and sector erases here, where they run
// with interrupts masked.
//
// This core answers them from the IRQn_MBX1A interrupt rather than from the main
// loop. Two reasons:
//   * core1 busy-waits for the answer, and eeprom_emulation.c holds core1's
//     interrupts masked across each flash op, so RPC service latency turns
//     directly into PDI-ISR blocking on core1. An ISR bounds it; the main loop
//     (tud_task, the CAN/UART pumps) does not.
//   * it needs no call site in the main loop, so the only integration point is
//     the single init call below.
//
// COST, and why the request granularity is one ROM API call: a sector erase
// masks this core's interrupts for tens of milliseconds -- USB will NAK and may
// drop the session, and MCAN RX can overrun. core1 therefore issues ONE request
// per sector, so interrupts come back between them, and refuses erases outright
// once its boot window closes (ecat/core1_ecat/src/ecat_flash.h). Programs are
// bounded at 512 bytes, a few hundred microseconds.
//
// SAFETY: every request is range-checked here against the emulated-EEPROM window
// alone. core1 cannot reach the application image or the bootloader through this
// path even if its own state machine goes wrong -- that check is on this side by
// design, not as a courtesy.

#if defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1

// Bring up the NOR (rom_xpi_nor_auto_config, which reconfigures XPI0 and is
// therefore this core's alone), publish the geometry into the channel, and arm
// the MBX1A doorbell.
//
// Ordering is not negotiable: AFTER publish_channel() (it writes into the
// channel) and BEFORE release_core1() (core1 reads the published geometry during
// ecat_hardware_init(), and a write to a still-gated mailbox is silently lost).
//
// A failure to bring the NOR up is not fatal here: the channel is left
// advertising "no flash server", and core1 keeps its emulated EEPROM read-only
// instead of stalling on requests nobody will answer.
void flash_server_init();

#else

inline void flash_server_init() {}

#endif

} // namespace librmcs::firmware::xcore
