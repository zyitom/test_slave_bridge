#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include "ch32h417.h"
}

#include "core/include/librmcs/data/datas.hpp"

// Board-hardware layer for the single CH32H417EVT variant, mirroring upstream
// rmcs_board's boards/<variant>/board_app split (kept as one file here since
// this board has no pro/lite variants). Everything hardware-specific -- which
// GPIO pins a peripheral uses, its alternate function, its kernel clock -- lives
// HERE, not in the board-agnostic drivers under can/ and uart/. The drivers are
// built from the port tables below, so adding a bus means adding a table entry.
namespace librmcs::firmware::board {

// One pin's routing. CH32H417 selects alternate functions per pin (like STM32's
// AFR), so GPIO_PinAFConfig() must run before GPIO_Init() -- omitting it leaves
// the pin on its default function and the peripheral silently unconnected.
struct PinConfig {
    GPIO_TypeDef* port;
    uint16_t pin_source; // GPIO_PinSourceN (an index, NOT the GPIO_Pin_N bitmask)
    uint8_t alternate;   // GPIO_AFn
};

// One physical CAN controller exposed by the board, in logical order (the first
// entry is can1, the second can2, ...).
//
// Valid pin routings for this part, from the EVT reference
// (EXAM/CAN/Networking/Common/hardware.c):
//   CAN1 TX: PA12(AF9) PB7(AF3) PB9(AF9) PD1(AF9) PA14(AF5)
//   CAN1 RX: PA11(AF9) PB6(AF3) PB8(AF9) PD0(AF9) PA13(AF5)
//   CAN2 TX: PB13(AF9) PB6(AF9)
//   CAN2 RX: PB12(AF9) PB5(AF9)
// PB8/PB9 are SWCLK/SWDIO on this package -- never route CAN there, it costs the
// debug interface (see PITFALLS.md).
struct CanPort {
    uint32_t base; // CANx_BASE
    IRQn_Type irq_num;
    PinConfig tx;
    PinConfig rx;
    data::DataId data_id;
    uint32_t bitrate;
};

// One USART exposed by the board, in logical order.
struct UartPort {
    uint32_t base; // USARTx_BASE
    IRQn_Type irq_num;
    PinConfig tx;
    PinConfig rx;
    data::DataId data_id;
    // Downlink field the host addresses runtime reconfiguration to, kept
    // distinct from data_id so a config patch cannot be confused with payload.
    // Must match the config_data_id in core/include/librmcs/spec/ch32_board/uart.hpp.
    data::DataId config_data_id;
    uint32_t baudrate; // power-on rate; the host may change it at runtime
    uint16_t max_receive_size;
};

// NOTE(bring-up): the pin choices below are still placeholders picked from the
// legal routings above; confirm against PUB/CH32H417SCH.pdf before trusting CAN
// or UART traffic. Everything else in these tables is verified.
inline constexpr CanPort kCanPorts[] = {
    {
        .base = CAN1_BASE,
        .irq_num = CAN1_RX0_IRQn,
        .tx = {GPIOA, GPIO_PinSource12, GPIO_AF9},
        .rx = {GPIOA, GPIO_PinSource11, GPIO_AF9},
        .data_id = data::DataId::kCan1,
        .bitrate = 1'000'000,
    },
    {
        .base = CAN2_BASE,
        .irq_num = CAN2_RX0_IRQn,
        .tx = {GPIOB, GPIO_PinSource13, GPIO_AF9},
        .rx = {GPIOB, GPIO_PinSource12, GPIO_AF9},
        .data_id = data::DataId::kCan2,
        .bitrate = 1'000'000,
    },
};
inline constexpr size_t kCanPortCount = sizeof(kCanPorts) / sizeof(kCanPorts[0]);

inline constexpr UartPort kUartPorts[] = {
    {
        .base = USART1_BASE,
        .irq_num = USART1_IRQn,
        .tx = {GPIOA, GPIO_PinSource9, GPIO_AF7},
        .rx = {GPIOA, GPIO_PinSource10, GPIO_AF7},
        .data_id = data::DataId::kUart1,
        .config_data_id = data::DataId::kUart1Config,
        .baudrate = 115'200,
        .max_receive_size = 64,
    },
    {
        .base = USART2_BASE,
        .irq_num = USART2_IRQn,
        .tx = {GPIOA, GPIO_PinSource2, GPIO_AF7},
        .rx = {GPIOA, GPIO_PinSource3, GPIO_AF7},
        .data_id = data::DataId::kUart2,
        .config_data_id = data::DataId::kUart2Config,
        .baudrate = 115'200,
        .max_receive_size = 64,
    },
};
inline constexpr size_t kUartPortCount = sizeof(kUartPorts) / sizeof(kUartPorts[0]);

// Kernel clock shared by CAN, USART and the timers on this part. NOT
// SystemCoreClock: on the V5F core that is the 400 MHz core clock, while
// peripherals hang off the ~100 MHz bus clock (SystemClock >> HPRE >> FPRE).
// The vendor's own USB_Timer_Init() derives from HCLK_Frequency the same way,
// which is the ground truth here -- using SystemCoreClock puts every prescaler
// out by the FPRE factor (4x on the stock 400 MHz clock tree).
uint32_t peripheral_clock();

// Pinmux + peripheral clock gating. Both return the peripheral kernel clock in
// Hz so the driver can compute its own prescaler.
uint32_t init_can(const CanPort& port);
uint32_t init_uart(const UartPort& port);

} // namespace librmcs::firmware::board
