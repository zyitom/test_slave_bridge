#include "board.h"

#include <assert.h>
#include <stdint.h>

#include <hpm_batt_iomux.h>
#include <hpm_clock_drv.h>
#include <hpm_common.h>
#include <hpm_debug_console.h>
#include <hpm_gpio_drv.h>
#include <hpm_gpiom_drv.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_ioc_regs.h>
#include <hpm_iomux.h>
#include <hpm_pcfg_drv.h>
#include <hpm_pllctlv2_drv.h>
#include <hpm_pmic_iomux.h>
#include <hpm_pmp_drv.h>
#include <hpm_soc.h>
#include <hpm_sysctl_drv.h>
#include <hpm_usb_drv.h>

/* Pin-level configuration is copied from the SDK hpm6e00evk board files
 * (boards/hpm6e00evk/{board.c,pinmux.c}); the hardware is that design. */

#if defined(FLASH_XIP) && FLASH_XIP
__attribute__((section(".nor_cfg_option"), used))
const uint32_t kOption[4] = {0xfcf90002, 0x00000007, 0x00001000, 0x0};
#endif

#if defined(FLASH_UF2) && FLASH_UF2
ATTR_PLACE_AT(".uf2_signature")
__attribute__((used)) const uint32_t kUf2Signature = BOARD_UF2_SIGNATURE;
#endif

static inline void board_init_clock(void);
static inline void init_esc_pins(void);

/* Console: UART0 on PA00/PA01, routed to the on-board FT2232 (DEBUGUART0). */
#define BOARD_CONSOLE_UART_BASE     HPM_UART0
#define BOARD_CONSOLE_UART_CLK_NAME clock_uart0
#define BOARD_CONSOLE_BAUDRATE      (115200UL)

void board_init_console(void) {
    console_config_t cfg;

    /* Configure the pin function before enabling the clock, otherwise the rx
     * level change during muxing can be latched as a spurious byte. */
    HPM_IOC->PAD[IOC_PAD_PA00].FUNC_CTL = IOC_PA00_FUNC_CTL_UART0_TXD;
    HPM_IOC->PAD[IOC_PAD_PA01].FUNC_CTL = IOC_PA01_FUNC_CTL_UART0_RXD;

    clock_add_to_group(BOARD_CONSOLE_UART_CLK_NAME, 0);

    cfg.type = CONSOLE_TYPE_UART;
    cfg.base = (uint32_t)BOARD_CONSOLE_UART_BASE;
    cfg.src_freq_in_hz = clock_get_frequency(BOARD_CONSOLE_UART_CLK_NAME);
    cfg.baudrate = BOARD_CONSOLE_BAUDRATE;

    if (console_init(&cfg) != status_success) {
        while (1) {}
    }
}

static void board_park_leds_off(void) {
    /* Drive every board LED to its OFF state at boot so nothing glows before the
     * fieldbus core takes ownership. All 15 GPIO LED pads were verified by the
     * GPIO LED scan (see boards/hpm6e8y/GPIO_LED_REVERSE_ENGINEERING.md). Polarity
     * is mixed: the main RGB is active-LOW (common-anode, OFF = drive high) while
     * every CAN/EtherCAT indicator is active-HIGH (OFF = drive low). Undriven
     * pads float high, which is why EtherCAT0 yellow (PA25) glowed at power-up.
     *
     * These must be real push-pull outputs: a weak internal pull cannot hold an
     * LED line. This runs before board_init_clock(), so the GPIO peripheral clock
     * is not up yet -- enable it here or the output writes silently no-op. */
    clock_add_to_group(clock_gpio, 0);

    /* {GPIO port/bank index, pin, off level}. Bank index: A=0, B=1, C=2, E=4. */
    static const struct {
        uint8_t bank;
        uint8_t pin;
        uint8_t off_level;
    } leds[] = {
        {GPIO_DO_GPIOE, 5, 1}, {GPIO_DO_GPIOE, 4, 1}, {GPIO_DO_GPIOE, 3, 1}, /* main RGB R/G/B */
        {GPIO_DO_GPIOC, 26, 0}, {GPIO_DO_GPIOC, 27, 0},                      /* CAN0 green/blue */
        {GPIO_DO_GPIOE, 0, 0}, {GPIO_DO_GPIOE, 2, 0},                        /* CAN1 green/blue */
        {GPIO_DO_GPIOA, 9, 0}, {GPIO_DO_GPIOB, 0, 0},                        /* CAN2 green/blue */
        {GPIO_DO_GPIOB, 2, 0}, {GPIO_DO_GPIOB, 3, 0},                        /* CAN3 green/blue */
        /* EtherCAT0 yellow (PA25) is active-LOW (lit when driven low; its scan
         * frame was stage 'L' and it glowed at power-up), so OFF = drive high.
         * EtherCAT1 yellow (PA28) is active-high like the other indicators. */
        {GPIO_DO_GPIOA, 25, 1}, {GPIO_DO_GPIOA, 28, 0},                      /* EtherCAT0/1 yellow */
        {GPIO_DO_GPIOC, 20, 0}, {GPIO_DO_GPIOC, 21, 0},                      /* EtherCAT mid grn/red */
    };

    for (uint32_t i = 0; i < sizeof(leds) / sizeof(leds[0]); ++i) {
        const uint32_t pad = ((uint32_t)leds[i].bank * 32U) + leds[i].pin;
        HPM_IOC->PAD[pad].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0); /* GPIO */
        HPM_IOC->PAD[pad].PAD_CTL = 0;
        gpiom_set_pin_controller(HPM_GPIOM, leds[i].bank, leds[i].pin, gpiom_soc_gpio0);
        gpio_set_pin_output_with_initial(HPM_GPIO0, leds[i].bank, leds[i].pin, leds[i].off_level);
    }
}

bool board_check_bootloader_force_stay_requested(void) {
    /* User key KEYA = PB24, pressed = low. */
    const uint32_t pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    clock_add_to_group(clock_gpio, 0);

    HPM_IOC->PAD[IOC_PAD_PB24].FUNC_CTL = IOC_PB24_FUNC_CTL_GPIO_B_24;
    HPM_IOC->PAD[IOC_PAD_PB24].PAD_CTL = pad_ctl;

    gpiom_set_pin_controller(HPM_GPIOM, GPIOM_ASSIGN_GPIOB, 24, gpiom_soc_gpio0);
    gpio_set_pin_input(HPM_GPIO0, GPIO_DI_GPIOB, 24);

    bool pressed = true;
    for (uint32_t sample_index = 0; sample_index < 4; ++sample_index) {
        board_delay_us(250);
        if (gpio_read_pin(HPM_GPIO0, GPIO_DI_GPIOB, 24) != 0U) {
            pressed = false;
            break;
        }
    }
    return pressed;
}

void board_init(void) {
    board_park_leds_off();
    board_init_clock();
    board_init_console();
    board_init_pmp();
}

void board_init_core1(void) {
    clock_update_core_clock();
    /* No console on core1: UART1 (the SDK's core1 console) is this board's
     * fieldbus data UART. The fieldbus application must not printf. */
    board_init_pmp();
}

void board_init_pmp(void) {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t length;
    pmp_entry_t pmp_entry[16] = {0};
    uint8_t index = 0;

    pmp_entry[index].pmp_addr = 0xFFFFFFFF;
    pmp_entry[index].pmp_cfg.val =
        PMP_CFG(READ_EN, WRITE_EN, EXECUTE_EN, ADDR_MATCH_NAPOT, REG_UNLOCK);
    index++;

    /* Non-cacheable data region (DMA buffers). Linker-script symbol names.
     * NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
    extern uint32_t __noncacheable_start__[];
    extern uint32_t __noncacheable_end__[];
    /* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */
    start_addr = (uint32_t)__noncacheable_start__;
    end_addr = (uint32_t)__noncacheable_end__;
    length = end_addr - start_addr;
    if (length > 0) {
        assert((length & (length - 1U)) == 0U);
        assert((start_addr & (length - 1U)) == 0U);
        pmp_entry[index].pmp_addr = PMP_NAPOT_ADDR(start_addr, length);
        pmp_entry[index].pmp_cfg.val =
            PMP_CFG(READ_EN, WRITE_EN, EXECUTE_EN, ADDR_MATCH_NAPOT, REG_UNLOCK);
        pmp_entry[index].pma_addr = PMA_NAPOT_ADDR(start_addr, length);
        pmp_entry[index].pma_cfg.val =
            PMA_CFG(ADDR_MATCH_NAPOT, MEM_TYPE_MEM_NON_CACHE_BUF, AMO_EN);
        index++;
    }

    /* SHARE_RAM: non-cacheable + AMO on BOTH cores -- the cross-core rings of
     * the EtherCAT bridge (ecat/common/xcore_ring.hpp) rely on this.
     * NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
    extern uint32_t __share_mem_start__[];
    extern uint32_t __share_mem_end__[];
    /* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */
    start_addr = (uint32_t)__share_mem_start__;
    end_addr = (uint32_t)__share_mem_end__;
    length = end_addr - start_addr;
    if (length > 0) {
        assert((length & (length - 1U)) == 0U);
        assert((start_addr & (length - 1U)) == 0U);
        pmp_entry[index].pmp_addr = PMP_NAPOT_ADDR(start_addr, length);
        pmp_entry[index].pmp_cfg.val =
            PMP_CFG(READ_EN, WRITE_EN, EXECUTE_EN, ADDR_MATCH_NAPOT, REG_UNLOCK);
        pmp_entry[index].pma_addr = PMA_NAPOT_ADDR(start_addr, length);
        pmp_entry[index].pma_cfg.val =
            PMA_CFG(ADDR_MATCH_NAPOT, MEM_TYPE_MEM_NON_CACHE_BUF, AMO_EN);
        index++;
    }

    pmp_config(&pmp_entry[0], index);
}

static inline void board_init_clock(void) {
    const uint32_t cpu0_freq = clock_get_frequency(clock_cpu0);
    if (cpu0_freq == PLLCTL_SOC_PLL_REFCLK_FREQ) {
        /* Configure the External OSC ramp-up time: ~9ms */
        pllctlv2_xtal_set_rampup_time(HPM_PLLCTLV2, 32UL * 1000UL * 9U);

        /* Select clock setting preset1 */
        sysctl_clock_set_preset(HPM_SYSCTL, 2);
    }

    /* Group 0: core0 domain (fabric, flash, DMA, EtherCAT-side resources). */
    clock_add_to_group(clock_cpu0, 0);
    clock_add_to_group(clock_mchtmr0, 0);
    clock_add_to_group(clock_ahb0, 0);
    clock_add_to_group(clock_axif, 0);
    clock_add_to_group(clock_axis, 0);
    clock_add_to_group(clock_axic, 0);
    clock_add_to_group(clock_axin, 0);
    clock_add_to_group(clock_rom0, 0);
    clock_add_to_group(clock_xpi0, 0);
    clock_add_to_group(clock_lmm0, 0);
    clock_add_to_group(clock_lmm1, 0);
    clock_add_to_group(clock_ram0, 0);
    clock_add_to_group(clock_ram1, 0);
    clock_add_to_group(clock_hdma, 0);
    clock_add_to_group(clock_xdma, 0);
    clock_add_to_group(clock_gpio, 0);
    clock_add_to_group(clock_ptpc, 0);
    /* Connect Group0 to CPU0 */
    clock_connect_group_to_cpu(0, 0);

    /* Group 1: core1 domain (fieldbus peripherals live with the fieldbus
     * core: MCAN4 + UART1 + its machine timer). */
    clock_add_to_group(clock_cpu1, 1);
    clock_add_to_group(clock_mchtmr1, 1);
#if defined(RMCS_ECAT_CORE1_CAN_PIN_SCANNER) && RMCS_ECAT_CORE1_CAN_PIN_SCANNER
    clock_add_to_group(clock_can0, 1);
    clock_add_to_group(clock_can1, 1);
    clock_add_to_group(clock_can2, 1);
    clock_add_to_group(clock_can3, 1);
    clock_add_to_group(clock_can4, 1);
    clock_add_to_group(clock_can5, 1);
    clock_add_to_group(clock_can6, 1);
    clock_add_to_group(clock_can7, 1);
#elif (defined(RMCS_ECAT_CORE1_LED_PIN_SCANNER) && RMCS_ECAT_CORE1_LED_PIN_SCANNER)      \
    || (defined(RMCS_ECAT_CORE1_LED_CONFIRM) && RMCS_ECAT_CORE1_LED_CONFIRM)
    clock_add_to_group(clock_gpio, 1);
    clock_add_to_group(clock_can0, 1);
#else
    clock_add_to_group(clock_can4, 1);
#endif
    clock_add_to_group(clock_uart1, 1);
    /* Connect Group1 to CPU1 */
    clock_connect_group_to_cpu(1, 1);

    /* Bump up DCDC voltage to 1275mv */
    pcfg_dcdc_set_voltage(HPM_PCFG, 1275);
    pcfg_dcdc_switch_to_dcm_mode(HPM_PCFG);

    /* Set CPU clock to 600MHz */
    clock_set_source_divider(clock_cpu0, clk_src_pll0_clk0, 1);
    clock_set_source_divider(clock_cpu1, clk_src_pll0_clk0, 1);

    /* Pin AHB0 to a known 200 MHz (PLL1CLK0 800 MHz / 4). PTPC -- the shared
     * CAN timestamp timebase -- runs on AHB0; the app-layer CAN driver bakes
     * the resulting 5 ns PTPC step into a compile-time divisor
     * (board::kCanTimestampNsPerUs = 1000, see app/board_app.hpp) and asserts
     * the relationship at init. Keep these three places consistent. */
    clock_set_source_divider(clock_ahb0, clk_src_pll1_clk0, 4);

    /* Both machine timers run at 4 MHz (24 MHz OSC / 6): the shared app-layer
     * Timer (app/src/timer) and the protocol session lease constants assume
     * 0.25 us ticks on every board. */
    clock_set_source_divider(clock_mchtmr0, clk_src_osc24m, 6);
    clock_set_source_divider(clock_mchtmr1, clk_src_osc24m, 6);

    clock_update_core_clock();
}

void board_init_usb(void) {
    /* USB0_ID / USB0_OC / USB0_PWR */
    HPM_IOC->PAD[IOC_PAD_PF22].FUNC_CTL = IOC_PF22_FUNC_CTL_USB0_ID;
    HPM_IOC->PAD[IOC_PAD_PF23].FUNC_CTL = IOC_PF23_FUNC_CTL_USB0_OC;
    HPM_IOC->PAD[IOC_PAD_PF19].FUNC_CTL = IOC_PF19_FUNC_CTL_USB0_PWR;

    clock_add_to_group(clock_usb0, 0);

    usb_hcd_set_power_ctrl_polarity(HPM_USB0, true);
    /* Wait for the USB_PWR pin controlled vbus power to stabilize. */
    board_delay_ms(100);
}

void board_init_ethercat(ESC_Type* ptr) {
    (void)ptr;

    clock_add_to_group(clock_esc0, 0);

    init_esc_pins();
    /* Keep the (shared) ECAT PHY reset asserted; the port layer releases it. */
    gpio_set_pin_output_with_initial(
        BOARD_ECAT_PHY0_RESET_GPIO, BOARD_ECAT_PHY0_RESET_GPIO_PORT_INDEX,
        BOARD_ECAT_PHY0_RESET_PIN_INDEX, BOARD_ECAT_PHY_RESET_LEVEL);
    gpio_set_pin_output_with_initial(
        BOARD_ECAT_PHY1_RESET_GPIO, BOARD_ECAT_PHY1_RESET_GPIO_PORT_INDEX,
        BOARD_ECAT_PHY1_RESET_PIN_INDEX, BOARD_ECAT_PHY_RESET_LEVEL);
}

static inline void init_esc_pins(void) {
    HPM_IOC->PAD[IOC_PAD_PA09].FUNC_CTL = IOC_PA09_FUNC_CTL_ESC0_REFCK;
    HPM_IOC->PAD[IOC_PAD_PA30].FUNC_CTL = IOC_PA30_FUNC_CTL_ESC0_MDIO;
    HPM_IOC->PAD[IOC_PAD_PA31].FUNC_CTL = IOC_PA31_FUNC_CTL_ESC0_MDC;

    /* PHY reset line as GPIO (see board_init_ethercat). */
    HPM_IOC->PAD[IOC_PAD_PA10].FUNC_CTL = IOC_PA10_FUNC_CTL_GPIO_A_10;

    /* NMII_LINK0 (port A link), matches BOARD_ECAT_NMII_LINK0_CTRL_INDEX. */
    HPM_IOC->PAD[IOC_PAD_PA15].FUNC_CTL = IOC_PA15_FUNC_CTL_ESC0_CTR_3;
    /* NMII_LINK1 (port B link), matches BOARD_ECAT_NMII_LINK1_CTRL_INDEX. */
    HPM_IOC->PAD[IOC_PAD_PA11].FUNC_CTL = IOC_PA11_FUNC_CTL_ESC0_CTR_0;
    /* NOTE: the EVK-derived ESC RUN/ERROR LED mux is deliberately NOT applied on
     * this board. The GPIO LED scan proved PE03 = Main RGB blue and PE02 = CAN1
     * blue indicator, not EtherCAT status LEDs. Re-muxing them to ESC0_CTR here
     * made those LEDs glow after board_turnoff_rgb_led()/init_led_pins() had
     * turned them off. The ESC still computes RUN/ERROR internally; it just is
     * not routed to a pad. Re-add a real ESC LED pad here once the true EtherCAT
     * pinout for this board is known. */

    /* ESC port A (MII) */
    HPM_IOC->PAD[IOC_PAD_PA24].FUNC_CTL = IOC_PA24_FUNC_CTL_ESC0_P0_TXCK;
    HPM_IOC->PAD[IOC_PAD_PA29].FUNC_CTL = IOC_PA29_FUNC_CTL_ESC0_P0_TXEN;
    HPM_IOC->PAD[IOC_PAD_PA25].FUNC_CTL = IOC_PA25_FUNC_CTL_ESC0_P0_TXD_0;
    HPM_IOC->PAD[IOC_PAD_PA26].FUNC_CTL = IOC_PA26_FUNC_CTL_ESC0_P0_TXD_1;
    HPM_IOC->PAD[IOC_PAD_PA27].FUNC_CTL = IOC_PA27_FUNC_CTL_ESC0_P0_TXD_2;
    HPM_IOC->PAD[IOC_PAD_PA28].FUNC_CTL = IOC_PA28_FUNC_CTL_ESC0_P0_TXD_3;
    HPM_IOC->PAD[IOC_PAD_PA21].FUNC_CTL = IOC_PA21_FUNC_CTL_ESC0_P0_RXCK;
    HPM_IOC->PAD[IOC_PAD_PA16].FUNC_CTL = IOC_PA16_FUNC_CTL_ESC0_P0_RXDV;
    HPM_IOC->PAD[IOC_PAD_PA23].FUNC_CTL = IOC_PA23_FUNC_CTL_ESC0_P0_RXER;
    HPM_IOC->PAD[IOC_PAD_PA17].FUNC_CTL = IOC_PA17_FUNC_CTL_ESC0_P0_RXD_0;
    HPM_IOC->PAD[IOC_PAD_PA18].FUNC_CTL = IOC_PA18_FUNC_CTL_ESC0_P0_RXD_1;
    HPM_IOC->PAD[IOC_PAD_PA19].FUNC_CTL = IOC_PA19_FUNC_CTL_ESC0_P0_RXD_2;
    HPM_IOC->PAD[IOC_PAD_PA20].FUNC_CTL = IOC_PA20_FUNC_CTL_ESC0_P0_RXD_3;

    /* ESC port B (MII) */
    HPM_IOC->PAD[IOC_PAD_PB18].FUNC_CTL = IOC_PB18_FUNC_CTL_ESC0_P1_TXCK;
    HPM_IOC->PAD[IOC_PAD_PB23].FUNC_CTL = IOC_PB23_FUNC_CTL_ESC0_P1_TXEN;
    HPM_IOC->PAD[IOC_PAD_PB19].FUNC_CTL = IOC_PB19_FUNC_CTL_ESC0_P1_TXD_0;
    HPM_IOC->PAD[IOC_PAD_PB20].FUNC_CTL = IOC_PB20_FUNC_CTL_ESC0_P1_TXD_1;
    HPM_IOC->PAD[IOC_PAD_PB21].FUNC_CTL = IOC_PB21_FUNC_CTL_ESC0_P1_TXD_2;
    HPM_IOC->PAD[IOC_PAD_PB22].FUNC_CTL = IOC_PB22_FUNC_CTL_ESC0_P1_TXD_3;
    HPM_IOC->PAD[IOC_PAD_PB17].FUNC_CTL = IOC_PB17_FUNC_CTL_ESC0_P1_RXCK;
    HPM_IOC->PAD[IOC_PAD_PB12].FUNC_CTL = IOC_PB12_FUNC_CTL_ESC0_P1_RXDV;
    HPM_IOC->PAD[IOC_PAD_PA22].FUNC_CTL = IOC_PA22_FUNC_CTL_ESC0_P1_RXER;
    HPM_IOC->PAD[IOC_PAD_PB13].FUNC_CTL = IOC_PB13_FUNC_CTL_ESC0_P1_RXD_0;
    HPM_IOC->PAD[IOC_PAD_PB14].FUNC_CTL = IOC_PB14_FUNC_CTL_ESC0_P1_RXD_1;
    HPM_IOC->PAD[IOC_PAD_PB15].FUNC_CTL = IOC_PB15_FUNC_CTL_ESC0_P1_RXD_2;
    HPM_IOC->PAD[IOC_PAD_PB16].FUNC_CTL = IOC_PB16_FUNC_CTL_ESC0_P1_RXD_3;

    /* ESC SYNC0 output (unused in free-run; kept for parity with the EVK). */
    HPM_IOC->PAD[IOC_PAD_PE06].FUNC_CTL = IOC_PE06_FUNC_CTL_ESC0_EVTO_0;
}

void board_delay_us(uint32_t us) { clock_cpu_delay_us(us); }

void board_delay_ms(uint32_t ms) { clock_cpu_delay_ms(ms); }
