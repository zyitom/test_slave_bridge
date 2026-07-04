#ifndef ECAT_CONFIG_H
#define ECAT_CONFIG_H

/*
 * Hardware configuration consumed by the SDK's EtherCAT port layer
 * (samples/ethercat/port/hpm_ecat_hw.c and hpm_ecat_e2p_emulation.c).
 * Values follow the ecat_io sample for the hpm6e00evk board.
 */

#include <hpm_clock_drv.h> /* clock_gptmr0 (our board.h stays minimal) */

#include "board.h"

#define FLASH_ADDR_BASE   BOARD_FLASH_BASE_ADDRESS
#define FLASH_SECTOR_SIZE (0x1000U) /* 4096 */

/* Flash-emulated ESC EEPROM. Keep clear of the application image; the EVK
 * board file reserves an offset of 2 MiB for this purpose. */
#define FLASH_EEPROM_ADDR (BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR)
#define FLASH_EEPROM_SIZE (0x10000)

/* 1 ms tick required by the SSC (ECAT_CheckTimer). */
#define ECAT_TIMER_GPTMR     HPM_GPTMR0
#define ECAT_TIMER_GPTMR_CH  (0U)
#define ECAT_TIMER_GPTMR_CLK (clock_gptmr0)
#define ECAT_TIMER_GPTRM_IRQ IRQn_GPTMR0

/* Flash EEPROM emulation component configuration. */
#define ECAT_EEPROM_FLASH_OFFSET      (FLASH_EEPROM_ADDR)
#define ECAT_EEPROM_FLASH_SECTOR_SIZE (FLASH_SECTOR_SIZE)
#define ECAT_EEPROM_FLASH_SECTOR_CNT  (FLASH_EEPROM_SIZE / FLASH_SECTOR_SIZE)

/* Check Product Code / Revision in the emulated EEPROM; refresh it from the
 * built-in image when they do not match. */
#define ECAT_EEPROM_CHECK_PRODUCT_CODE_AND_REVISION (1)

/* Emulated EEPROM size in bytes. */
#define ECAT_EEPROM_SIZE_BYTE (16 * 1024) /* 16 KiB = 128 Kbit */

/* An ESC reset request resets the whole MCU, not just the slave controller:
 * this is a forwarding bridge, so a half-reset device is worse than a clean
 * reboot. */
#define ECAT_RESET_ESC_PERIPHERAL 0
#define ECAT_RESET_ESC_WITH_MCU   1

#endif /* ECAT_CONFIG_H */
