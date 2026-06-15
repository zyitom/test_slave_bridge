#pragma once

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

// This board exposes a single CAN-FD, a single UART and a plain GPIO RGB LED;
// it has no IMU, GPIO application, DBUS UART or USB speed switch.

// The HPM5321 USB always runs at high speed.
bool usb_use_high_speed();

// CAN ports in logical order (CAN0, CAN1, ...). CAN0 is the DM (Damiao) motor
// bus and runs CAN-FD; this board has no other CAN.
constexpr CanPort kCanPorts[] = {
    {HPM_MCAN0_BASE, IRQn_MCAN0, CanMode::kCanFd},
};

uint32_t init_can(MCAN_Type* ptr);
void can_irq_handler(size_t board_can_index);

// UART ports in logical order. This board has a single data UART (UART2) and no
// DBUS receiver.
constexpr UartPort kUartPorts[] = {
    {HPM_UART2_BASE, IRQn_UART2, HPM_DMA_SRC_UART2_TX, HPM_DMA_SRC_UART2_RX,
     data::DataId::kUart0, 921600, parity_none},
};

uint32_t init_uart(UART_Type* ptr);
void uart_irq_handler(size_t board_uart_index);

// Plain GPIO RGB LED, active-low (common-anode: drive the pad LOW to light the
// channel). The last template argument is active_high = false.
constexpr GpioPin kLedBluePin = make_gpio_pin<gpiom_soc_gpio0, 'A', 29, false>();
constexpr GpioPin kLedGreenPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 30, false>();
constexpr GpioPin kLedRedPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 31, false>();

void init_led_pins();

} // namespace librmcs::firmware::board
