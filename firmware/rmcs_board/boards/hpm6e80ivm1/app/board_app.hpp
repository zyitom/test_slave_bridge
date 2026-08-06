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

// Fieldbus (core1) application layer of the EtherCAT bridge. This board
// exposes one test CAN (MCAN4, the EVK's on-board transceiver port) and one
// test UART (UART1 on PY06/PY07), plus a plain GPIO RGB LED. The EtherCAT
// side (ESC, core0) is configured in ../board.c and does not appear here.

// CAN ports in logical order. CAN0 = MCAN4, classic 1 Mbps for bring-up; the
// on-board transceiver's STB pin is driven low (normal mode) by init_can().
constexpr CanPort kCanPorts[] = {
    {.base = HPM_MCAN4_BASE, .irq_num = IRQn_MCAN4, .mode = CanMode::kClassic},
};

// Table capacity and the number of controllers actually populated on this board.
// They differ only on boards whose directory serves more than one PCB
// (boards/hpm5321), where the table is sized for the larger variant and this
// count comes from the runtime identity. Here they are the same.
constexpr size_t kCanPortCapacity = std::size(kCanPorts);
constexpr size_t can_port_count() { return kCanPortCapacity; }
constexpr CanPort can_port(size_t index) { return kCanPorts[index]; }

uint32_t init_can(MCAN_Type* ptr);
void can_irq_handler(size_t board_can_index);

// MCAN message RAM on HPM6E80 must live in the 32 KiB AHB RAM at 0xF0200000.
// The core1 linker script exposes no .ahb_sram output section, so the board
// hands out fixed slices of that (otherwise unused) region instead of a
// section-placed array.
mcan_msg_buf_attr_t can_message_ram(size_t can_index);

// PTPC (the shared CAN timestamp timebase) runs on AHB0, pinned to 200 MHz in
// board.c: reported-nanosecond step is 5 ns, so true microseconds = reported
// nanoseconds / (200 * 5). The CAN driver asserts this against the clock tree
// at init -- if board.c changes the AHB0 divider, update this constant.
constexpr uint32_t kCanTimestampNsPerUs = 1000;

// UART ports in logical order: one test data UART (UART1, PY06/PY07 header).
constexpr UartPort kUartPorts[] = {
    {.base = HPM_UART1_BASE,
     .irq_num = IRQn_UART1,
     .dma_src_tx = HPM_DMA_SRC_UART1_TX,
     .dma_src_rx = HPM_DMA_SRC_UART1_RX,
     .data_id = data::DataId::kUart0,
     .config_data_id = data::DataId::kUart0Config,
     .baudrate = 921600,
     .parity = parity_none},
};

uint32_t init_uart(UART_Type* ptr);
void uart_irq_handler(size_t board_uart_index);

// The fieldbus application runs on core1: its machine timer is MCHTMR1
// (board.c clocks it at the 4 MHz the shared Timer driver expects).
constexpr clock_name_t kMchtmrClockName = clock_mchtmr1;

// DMA ring storage section for the shared UART driver. Core1 has no AHB SRAM
// section; the AXI SRAM non-cacheable region serves the same purpose (DMA
// coherent without manual cache maintenance).
#define LIBRMCS_DMA_BUFFER_SECTION ".noncacheable.non_init"

// Plain GPIO RGB LED (EVK RGB LED, active-high).
constexpr GpioPin kLedRedPin = make_gpio_pin<gpiom_soc_gpio0, 'E', 14, true>();
constexpr GpioPin kLedGreenPin = make_gpio_pin<gpiom_soc_gpio0, 'E', 15, true>();
constexpr GpioPin kLedBluePin = make_gpio_pin<gpiom_soc_gpio0, 'E', 4, true>();

// Accessor form of the three constants above. The shared LED driver calls these
// rather than reading the constants, because a board directory that serves more
// than one PCB has to pick its pads from the runtime identity
// (boards/hpm5321/app/board_app.hpp does). This board has one pinout, so these
// are constant folds.
constexpr GpioPin led_red_pin() { return kLedRedPin; }
constexpr GpioPin led_green_pin() { return kLedGreenPin; }
constexpr GpioPin led_blue_pin() { return kLedBluePin; }

// No per-CAN indicator LEDs on this board.
constexpr std::array<GpioPin, 0> kCanIndicatorPins{};
constexpr size_t can_indicator_count() { return kCanIndicatorPins.size(); }

void init_led_pins();
void init_can_indicator_pins();

} // namespace librmcs::firmware::board
