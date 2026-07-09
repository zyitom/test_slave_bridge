#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hpm_common.h>
#include <hpm_soc.h>

/*
 * RMCS EtherCAT bridge board "hpm6e8y" — HPM6E80 firmware on HPM6E00 chip,
 * hpm6e00evk-equivalent hardware. Differs from hpm6e80ivm1 ONLY in the XPI NOR
 * FCFG: uses option 1 (0xfcf90002) instead of option 0 (0xfcf90001).
 *
 * Follows the repo board API used by the shared bootloader (board_init,
 * board_init_usb(void), the force-stay hook, flash/XPI macros) and adds the
 * dual-core + EtherCAT hooks the bridge firmware needs (board_init_core1,
 * board_init_pmp, board_init_ethercat). Pin-level facts are taken verbatim
 * from the SDK hpm6e00evk board files, since the hardware is that design.
 */

#define BOARD_NAME          "RMCS_ECAT_Bridge_HPM6E8Y"
#define BOARD_UF2_SIGNATURE (0x0A4D5048UL)

/* Load/entry address of the secondary-core image (SDK board convention,
 * consumed by multicore_release_cpu in core0's ecat_main.c). */
#define SEC_CORE_IMG_START CORE1_ILM_LOCAL_BASE

#define BOARD_FLASH_BASE_ADDRESS (0x80000000UL)
#define BOARD_FLASH_SIZE         (16 * SIZE_1MB)
/* The app image must end below the EtherCAT flash-emulated EEPROM region at
 * offset 2 MiB (BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR); the bootloader caps
 * accepted image sizes with this. */
#define BOARD_APP_FLASH_END_OFFSET (0x200000UL)

/* XPI NOR configuration option (same flash as hpm6e00evk), used by the
 * bootloader flash writer and by the EtherCAT EEPROM-emulation port layer. */
#define BOARD_APP_XPI_NOR_XPI_BASE     (HPM_XPI0)
#define BOARD_APP_XPI_NOR_CFG_OPT_HDR  (0xfcf90002U)
#define BOARD_APP_XPI_NOR_CFG_OPT_OPT0 (0x00000007U)
#define BOARD_APP_XPI_NOR_CFG_OPT_OPT1 (0x00001000U)

/* EtherCAT definitions consumed by the SDK port layer
 * (samples/ethercat/port). Values match the hpm6e00evk reference design. */
#define BOARD_ECAT_SUPPORT_PORT1 (1)
#define BOARD_ECAT_SUPPORT_PORT2 (0)

#define BOARD_ECAT_SUPPORT_RUN_ERROR_LED (1)

/* TEMPORARY: the ESC pin mapping in board.c (init_esc_pins) is copied from the
 * hpm6e00evk and does not match this board -- the GPIO LED scan proved several of
 * those "MII" pads are actually LED pads. Bringing the ESC up therefore both
 * mis-drives the LEDs and cannot work. Skip the ESC bring-up on core0 until the
 * real EtherCAT pinout for this board is known; core1 (CAN/UART fieldbus) and the
 * USB DFU runtime still run. Set to 0 to restore EtherCAT once re-pinned. */
#define BOARD_ECAT_DISABLE_ESC_BRINGUP (1)

/* ESC port link signals are active-low on this hardware. */
#define BOARD_ECAT_PORT0_LINK_INVERT true
#define BOARD_ECAT_PORT1_LINK_INVERT true
#define BOARD_ECAT_PORT2_LINK_INVERT false

/* Both external PHYs share one reset line (PA10). */
#define BOARD_ECAT_PHY0_RESET_GPIO            HPM_GPIO0
#define BOARD_ECAT_PHY0_RESET_GPIO_PORT_INDEX GPIO_DO_GPIOA
#define BOARD_ECAT_PHY0_RESET_PIN_INDEX       (10)
#define BOARD_ECAT_PHY1_RESET_GPIO            HPM_GPIO0
#define BOARD_ECAT_PHY1_RESET_GPIO_PORT_INDEX GPIO_DO_GPIOA
#define BOARD_ECAT_PHY1_RESET_PIN_INDEX       (10)
#define BOARD_ECAT_PHY_RESET_LEVEL            (0)

/* Must match the ESC0_CTR_y pin functions configured in board.c
 * (init_esc_pins): NMII_LINK0 = PA15/CTR_3, NMII_LINK1 = PA11/CTR_0,
 * LED_RUN = PE03/CTR_1, LED_ERROR = PE02/CTR_6. */
#define BOARD_ECAT_NMII_LINK0_CTRL_INDEX 3
#define BOARD_ECAT_NMII_LINK1_CTRL_INDEX 0
#define BOARD_ECAT_LED_RUN_CTRL_INDEX    1
#define BOARD_ECAT_LED_ERROR_CTRL_INDEX  6

#define BOARD_ECAT_PHY_ADDR_OFFSET (0U)
#define BOARD_ECAT_PORT0_PHY_ADDR  (0U)
#define BOARD_ECAT_PORT1_PHY_ADDR  (1U)

/* Only referenced by the port layer when a real I2C EEPROM is used; this
 * project uses flash emulation, but the macros must exist. */
#define BOARD_ECAT_INIT_EEPROM_I2C     HPM_I2C1
#define BOARD_ECAT_INIT_EEPROM_I2C_CLK clock_i2c1

/* Flash offset of the emulated ESC EEPROM (see BOARD_APP_FLASH_END_OFFSET). */
#define BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR (0x200000)

#ifdef __cplusplus
extern "C" {
#endif

void board_init(void);
void board_init_core1(void);
void board_init_console(void);
void board_init_pmp(void);
void board_init_usb(void);
void board_init_ethercat(ESC_Type* ptr);

/* Bootloader-specific helper: hold the user key (PB24, pressed = low) through
 * reset to force the bootloader to stay in DFU mode. */
bool board_check_bootloader_force_stay_requested(void);

void board_delay_us(uint32_t us);
void board_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
