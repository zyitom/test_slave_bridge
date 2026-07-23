#pragma once

#include <cstdint>

extern "C" {
#include "ch32h417.h"
}

// Board-hardware layer for the single CH32H417EVT variant, mirroring upstream
// rmcs_board's boards/<variant>/board_app split (kept as one file here since
// this board has no pro/lite variants). Everything hardware-specific -- which
// GPIO pins a peripheral uses and its kernel clock -- lives HERE, not in the
// board-agnostic drivers under can/ and uart/. init_can/init_uart set up pinmux
// + clock and return the peripheral source clock in Hz.
namespace librmcs::firmware::board {

uint32_t init_can(CAN_TypeDef* can);
uint32_t init_uart(USART_TypeDef* usart);

} // namespace librmcs::firmware::board
