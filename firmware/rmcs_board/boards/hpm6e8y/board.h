#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hpm_common.h>
#include <hpm_soc.h>

/*
 * RMCS EtherCAT bridge board "hpm6e8y" - HPM6E80 firmware on HPM6E00 chip,
 * hpm6e00evk-equivalent hardware. Differs from hpm6e80ivm1 in the XPI NOR FCFG
 * and in EtherCAT PHY routing: this board uses the HPM6E*Y* on-die 100M PHYs.
 *
 * Follows the repo board API used by the shared bootloader (board_init,
 * board_init_usb(void), the force-stay hook, flash/XPI macros) and adds the
 * dual-core + EtherCAT hooks the bridge firmware needs (board_init_core1,
 * board_init_pmp, board_init_ethercat).
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
 * (samples/ethercat/port). */
#define BOARD_ECAT_SUPPORT_PORT1 (1)
#define BOARD_ECAT_SUPPORT_PORT2 (0)

/* The MDIO scanner and the ENET packet tester probe the EXTERNAL Realtek ENET0
 * PHY (MDC/MDIO on PF00/PF01). Core0's ESC bring-up (board_init_ethercat +
 * ecat_hardware_init in ecat_main.c) muxes the shared on-die PHY / clock / reset
 * pads (PA16-29, PW16-21, PV12, PW12) and, once it runs, the Realtek stops
 * answering MDIO -- an MDIO scan then sees ONLY the internal JL1111 PHYs and the
 * Realtek RJ45 stays dark. These diagnostic images do not need EtherCAT, so skip
 * the core0 ESC bring-up for them and leave the Realtek untouched. */
#if (defined(RMCS_ECAT_CORE1_MDIO_PIN_SCANNER) && RMCS_ECAT_CORE1_MDIO_PIN_SCANNER)        \
    || (defined(RMCS_ECAT_CORE1_ENET_PACKET_TESTER) && RMCS_ECAT_CORE1_ENET_PACKET_TESTER) \
    || (defined(RMCS_ECAT_CORE1_REALTEK_RESET_SCANNER) && RMCS_ECAT_CORE1_REALTEK_RESET_SCANNER)
# define BOARD_ECAT_DISABLE_ESC_BRINGUP (1)
#endif

/* RUN/ERROR status LEDs: the GPIO LED scan identified PC20 (green) / PC21 (red)
 * as the "EtherCAT middle" indicators, and both carry an ESC0_CTR alt function
 * (PC20 = CTR_2, PC21 = CTR_3), so the ESC drives them from the AL state machine.
 * board.c init_esc_pins muxes the pads; the indices below select the CTRs. */
#define BOARD_ECAT_SUPPORT_RUN_ERROR_LED (1)

/* ESC port link signals are active-low on this hardware. */
#define BOARD_ECAT_PORT0_LINK_INVERT true
#define BOARD_ECAT_PORT1_LINK_INVERT true
#define BOARD_ECAT_PORT2_LINK_INVERT false

/* The HPM6E*Y* on-die PHY reset inputs are internal PV/PW GPIO pads. */
#define BOARD_ECAT_PHY0_RESET_GPIO            HPM_GPIO0
#define BOARD_ECAT_PHY0_RESET_GPIO_PORT_INDEX GPIO_DO_GPIOV
#define BOARD_ECAT_PHY0_RESET_PIN_INDEX       (12)
#define BOARD_ECAT_PHY1_RESET_GPIO            HPM_GPIO0
#define BOARD_ECAT_PHY1_RESET_GPIO_PORT_INDEX GPIO_DO_GPIOW
#define BOARD_ECAT_PHY1_RESET_PIN_INDEX       (12)
#define BOARD_ECAT_PHY_RESET_LEVEL            (0)

/* Must match the ESC0_CTR_y functions configured in board.c. PA25 (CTR_0) and
 * PA28 (CTR_1) are the NMII_LINK sources; PC20 (CTR_2) and PC21 (CTR_3) are the
 * RUN/ERROR status LEDs. */
#define BOARD_ECAT_NMII_LINK0_CTRL_INDEX 1
#define BOARD_ECAT_NMII_LINK1_CTRL_INDEX 0
#define BOARD_ECAT_LED_RUN_CTRL_INDEX    2
#define BOARD_ECAT_LED_ERROR_CTRL_INDEX  3

#define BOARD_ECAT_PHY_ADDR_OFFSET (0U)
#define BOARD_ECAT_PORT0_PHY_ADDR  (2U)
#define BOARD_ECAT_PORT1_PHY_ADDR  (1U)
/* The board silkscreen remains EtherCAT0 = IN and EtherCAT1 = OUT. Internally,
 * the HPM ESC enumerates only when those physical links are reported to the
 * opposite logical ports: physical EtherCAT0/IN feeds ESC port 1 and physical
 * EtherCAT1/OUT feeds ESC port 0. Do not set this to 0 unless the board wiring
 * or the lower-level ESC port assignment changes; swap=0 was tested and the
 * host reported "No EtherCAT slave found". */
#define BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT (1)

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

/* Drive the ESC NMII_LINK for the two on-die PHYs from software (the on-die PHYs
 * do not feed a LINK pin to an ESC CTR input, so the SDK default leaves the ESC
 * seeing no link). Call AFTER ecat_hardware_init. See the definition in board.c. */
void board_ecat_set_internal_phy_link(bool port0_up, bool port1_up);

typedef struct board_ecat_phy_status {
    bool port0_read_ok;
    bool port0_link_up;
    uint16_t port0_bmcr;
    uint16_t port0_bmsr;
    uint16_t port0_id1;
    uint16_t port0_id2;
    uint16_t port0_rmsr_p7;
    bool port1_read_ok;
    bool port1_link_up;
    uint16_t port1_bmcr;
    uint16_t port1_bmsr;
    uint16_t port1_id1;
    uint16_t port1_id2;
    uint16_t port1_rmsr_p7;
} board_ecat_phy_status_t;

/* Read the two internal JL1111 PHYs through the proven ENET0 SMI path on
 * PA30/PA31, then restore PA30/PA31 to ESC0_MDIO/MDC. */
bool board_ecat_get_internal_phy_status(board_ecat_phy_status_t* status);

/* Force the two internal JL1111 PHYs to MII mode through the proven ENET0 SMI
 * path. Call after the EtherCAT port layer has released PHY reset. */
bool board_ecat_configure_internal_phy_mii_mode(void);

/* Refresh ESC NMII_LINK from the real PHY link state. Returns false if neither
 * internal PHY could be read, in which case the previous ESC link state is left
 * untouched. */
bool board_ecat_refresh_internal_phy_link(void);
bool board_ecat_wait_internal_phy_link(uint32_t timeout_ms);

/* Bootloader-specific helper: hold the user key (PB24, pressed = low) through
 * reset to force the bootloader to stay in DFU mode. */
bool board_check_bootloader_force_stay_requested(void);

void board_delay_us(uint32_t us);
void board_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
