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

// This board exposes a single CAN controller (classic or CAN-FD, host-configured),
// a single UART and a plain GPIO RGB LED; it has no IMU, GPIO application,
// DBUS UART or USB speed switch.

// The HPM5321 USB always runs at high speed.
bool usb_use_high_speed();

// CAN ports in logical order (CAN0, CAN1, ...). CAN0 is the DM (Damiao) motor
// bus and runs CAN-FD; this board has no other CAN.
constexpr CanPort kCanPorts[] = {
    {.base = HPM_MCAN0_BASE, .irq_num = IRQn_MCAN0, .mode = CanMode::kClassic},
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

// UART ports in logical order. This board has a single data UART (UART2) and no
// DBUS receiver.
constexpr UartPort kUartPorts[] = {
    {
        .base = HPM_UART2_BASE,
        .irq_num = IRQn_UART2,
        .dma_src_tx = HPM_DMA_SRC_UART2_TX,
        .dma_src_rx = HPM_DMA_SRC_UART2_RX,
        .data_id = data::DataId::kUart0,
        .config_data_id = data::DataId::kUart0Config,
        .baudrate = 921600,
        .parity = parity_none,
    },
};

uint32_t init_uart(UART_Type* ptr);
void uart_irq_handler(size_t board_uart_index);

// Plain GPIO RGB LED, active-low (common-anode: drive the pad LOW to light the
// channel). The last template argument is active_high = false.
constexpr GpioPin kLedBluePin = make_gpio_pin<gpiom_soc_gpio0, 'A', 29, false>();
constexpr GpioPin kLedGreenPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 30, false>();
constexpr GpioPin kLedRedPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 31, false>();

// This board has no per-CAN indicator LEDs: the table is empty and the shared
// Led driver skips the indicator logic entirely.
constexpr std::array<GpioPin, 0> kCanIndicatorPins{};

void init_led_pins();
void init_can_indicator_pins();

} // namespace librmcs::firmware::board
