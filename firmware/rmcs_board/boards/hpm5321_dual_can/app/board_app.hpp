#pragma once

#include <array>
#include <cstddef>

#include <hpm_clock_drv.h>
#include <hpm_common.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_regs.h>
#include <hpm_mcan_soc.h>
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

// MCAN message RAM region for the CAN controller at the given logical index
// (an .ahb_sram-placed array on this SoC, see board_app.cpp).
mcan_msg_buf_attr_t can_message_ram(size_t can_index);

// PTPC (the shared CAN timestamp timebase) runs on the 160 MHz AHB clock:
// reported-nanosecond step is 6 ns, so true microseconds = reported
// nanoseconds / (160 * 6). The CAN driver asserts this against the clock tree
// at init.
constexpr uint32_t kCanTimestampNsPerUs = 960;

// The application runs on core0: its machine timer is MCHTMR0, clocked at the
// 4 MHz the shared Timer driver expects (board.c).
constexpr clock_name_t kMchtmrClockName = clock_mchtmr0;

// DMA ring storage section for the shared UART driver: AHB SRAM is naturally
// non-cached on this SoC.
#define LIBRMCS_DMA_BUFFER_SECTION ".ahb_sram"

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
