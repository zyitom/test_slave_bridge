#include "board_app.hpp"

#include <cstdint>

#include <hpm_clock_drv.h>
#include <hpm_ioc_regs.h>
#include <hpm_iomux.h>
#include <hpm_mcan_regs.h>
#include <hpm_soc.h>
#include <hpm_soc_irq.h>
#include <hpm_uart_regs.h>

namespace librmcs::firmware::board {
namespace {

uint32_t init_can_clock(MCAN_Type* ptr) {
    if (ptr == HPM_MCAN0) {
        clock_add_to_group(clock_can0, 0);
        clock_set_source_divider(clock_can0, clk_src_pll1_clk0, 10);
        return clock_get_frequency(clock_can0);
    }
    return 0;
}

uint32_t init_uart_clock(UART_Type* ptr) {
    if (ptr == HPM_UART2) {
        clock_add_to_group(clock_uart2, 0);
        return clock_get_frequency(clock_uart2);
    }
    return 0;
}

} // namespace

uint32_t init_can(MCAN_Type* ptr) {
    if (ptr == HPM_MCAN0) {
        HPM_IOC->PAD[IOC_PAD_PA01].FUNC_CTL = IOC_PA01_FUNC_CTL_MCAN0_RXD;
        HPM_IOC->PAD[IOC_PAD_PA00].FUNC_CTL = IOC_PA00_FUNC_CTL_MCAN0_TXD;
    }
    return init_can_clock(ptr);
}

uint32_t init_uart(UART_Type* ptr) {
    constexpr uint32_t tx_pad = IOC_PAD_PAD_CTL_PE_SET(1) | // Pull enable
                                IOC_PAD_PAD_CTL_PS_SET(1);  // Pull select - Pull up
    constexpr uint32_t rx_pad = IOC_PAD_PAD_CTL_PE_SET(1) | // Pull enable
                                IOC_PAD_PAD_CTL_PS_SET(1) | // Pull select - Pull up
                                IOC_PAD_PAD_CTL_HYS_SET(1); // Enable Schmitt trigger
    if (ptr == HPM_UART2) {
        HPM_IOC->PAD[IOC_PAD_PB09].FUNC_CTL = IOC_PB09_FUNC_CTL_UART2_RXD;
        HPM_IOC->PAD[IOC_PAD_PB09].PAD_CTL = rx_pad;

        HPM_IOC->PAD[IOC_PAD_PB08].FUNC_CTL = IOC_PB08_FUNC_CTL_UART2_TXD;
        HPM_IOC->PAD[IOC_PAD_PB08].PAD_CTL = tx_pad;
    }
    return init_uart_clock(ptr);
}

void init_led_pins() {
    for (const auto& pin : {kLedRedPin, kLedGreenPin, kLedBluePin}) {
        pin.configure_controller();
        pin.configure_ioc_function();
        pin.configure_pad_control(0);
        pin.set_active(false);
        pin.configure_as_output();
    }
}

void init_can_indicator_pins() {
    // No per-CAN indicator LEDs on this board.
}

bool usb_use_high_speed() { return true; }

SDK_DECLARE_EXT_ISR_M(IRQn_MCAN0, can0_isr)
void can0_isr() { can_irq_handler(0); }

SDK_DECLARE_EXT_ISR_M(IRQn_UART2, uart0_isr)
void uart0_isr() { uart_irq_handler(0); }

} // namespace librmcs::firmware::board
