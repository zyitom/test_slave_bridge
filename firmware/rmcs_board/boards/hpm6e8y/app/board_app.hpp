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
// exposes all four physical CAN ports (CAN0..CAN3 = MCAN0..MCAN3; see kCanPorts
// for pin routing) and one test UART (UART1 on PY06/PY07), plus a plain GPIO RGB
// LED. The EtherCAT side (ESC, core0) is configured in ../board.c and does not
// appear here.

// USB0 uses the HPM6E80 high-speed device controller and PHY.
bool usb_use_high_speed();

// CAN ports in logical order, mapped to the four physical silk ports CAN0..CAN3
// (= MCAN0..MCAN3). Pin routing recovered by the CAN pin scanner and recorded in
// CAN_PIN_REVERSE_ENGINEERING.md:
//   CAN0 = MCAN0  TX PC00 / RX PC01
//   CAN1 = MCAN1  TX PB05 / RX PB04
//   CAN2 = MCAN2  TX PD08 / RX PD09
//   CAN3 = MCAN3  TX PD15 / RX PD14
// All four run CAN-FD (1 Mbps arbitration / 5 Mbps data, BRS on). FD mode is a
// strict superset -- classic frames still work -- and is required by the CANFD
// loopback stress test that jumpers CAN0<->CAN1 and CAN2<->CAN3.
constexpr CanPort kCanPorts[] = {
    {.base = HPM_MCAN0_BASE, .irq_num = IRQn_MCAN0, .mode = CanMode::kCanFd},
    {.base = HPM_MCAN1_BASE, .irq_num = IRQn_MCAN1, .mode = CanMode::kCanFd},
    {.base = HPM_MCAN2_BASE, .irq_num = IRQn_MCAN2, .mode = CanMode::kCanFd},
    {.base = HPM_MCAN3_BASE, .irq_num = IRQn_MCAN3, .mode = CanMode::kCanFd},
};

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

// Main RGB LED, active-LOW (common-anode: drive the pad low to light it). Pads
// verified by the GPIO LED scan (see GPIO_LED_REVERSE_ENGINEERING.md): red=PE05,
// green=PE04, blue=PE03. NOTE: these same pads are ESC0_CTR outputs in the
// EVK-derived board.c EtherCAT bring-up (init_esc_pins); that pinmux does not
// match this board and is temporary, so once EtherCAT is re-pinned for real
// hardware this overlap must be resolved.
constexpr GpioPin kLedRedPin = make_gpio_pin<gpiom_soc_gpio0, 'E', 5, false>();
constexpr GpioPin kLedGreenPin = make_gpio_pin<gpiom_soc_gpio0, 'E', 4, false>();
constexpr GpioPin kLedBluePin = make_gpio_pin<gpiom_soc_gpio0, 'E', 3, false>();

// This board DOES have per-CAN indicator LEDs (green+blue per port); the GPIO LED
// scan confirmed CAN0 green=PC26, CAN1 blue=PE02, CAN2 green=PA09/blue=PB00,
// CAN3 green=PB02/blue=PB03. The full green+blue-per-port mapping is still being
// scanned, so they are not wired up as indicators yet.
constexpr std::array<GpioPin, 0> kCanIndicatorPins{};

void init_led_pins();
void init_can_indicator_pins();

} // namespace librmcs::firmware::board
