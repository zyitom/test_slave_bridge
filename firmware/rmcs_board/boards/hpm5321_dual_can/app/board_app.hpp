#pragma once

#include <array>
#include <cstddef>

#include <hpm_common.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_regs.h>
#include <hpm_soc.h>
#include <hpm_soc_irq.h>
#include <hpm_uart_regs.h>

#include "firmware/rmcs_board/app/src/can/can_port.hpp"
#include "firmware/rmcs_board/app/src/gpio/gpio_pin.hpp"
#include "firmware/rmcs_board/app/src/uart/uart_port.hpp"

namespace librmcs::firmware::board {

bool usb_use_high_speed();

constexpr CanPort kCanPorts[] = {
    {HPM_MCAN0_BASE, IRQn_MCAN0, CanMode::kCanFd},
    {HPM_MCAN3_BASE, IRQn_MCAN3, CanMode::kCanFd},
};

uint32_t init_can(MCAN_Type* ptr);
void can_irq_handler(size_t board_can_index);

constexpr UartPort kUartPorts[] = {
    {HPM_UART2_BASE, IRQn_UART2, HPM_DMA_SRC_UART2_TX, HPM_DMA_SRC_UART2_RX,
     data::DataId::kUart0, 921600, parity_none},
};

uint32_t init_uart(UART_Type* ptr);
void uart_irq_handler(size_t board_uart_index);

constexpr GpioPin kLedBluePin = make_gpio_pin<gpiom_soc_gpio0, 'A', 26, false>();
constexpr GpioPin kLedGreenPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 27, false>();
constexpr GpioPin kLedRedPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 28, false>();

// CAN bus indicator LEDs -- one per CAN controller, in the same logical order
// as kCanPorts; active-high (LED anode to pin). The shared Led driver builds
// the indicator logic from this table, so a board without indicator LEDs just
// provides an empty table.
constexpr std::array kCanIndicatorPins{
    make_gpio_pin<gpiom_soc_gpio0, 'B', 14, true>(),
    make_gpio_pin<gpiom_soc_gpio0, 'B', 15, true>(),
};

void init_led_pins();
void init_can_indicator_pins();

} // namespace librmcs::firmware::board
