#include "firmware/ch32_board/app/src/uart/uart.hpp"

namespace librmcs::firmware::uart {

// Thin ISR shims routing each USART interrupt to its array element's
// irq_handler(). Plain __attribute__((interrupt)) so GCC emits mret.

static_assert(kUartCount == 2, "ISR shims below must cover every board::kUartPorts entry");

extern "C" void USART1_IRQHandler(void) __attribute__((interrupt()));
extern "C" void USART1_IRQHandler(void) { uart_array[0]->irq_handler(); }

extern "C" void USART2_IRQHandler(void) __attribute__((interrupt()));
extern "C" void USART2_IRQHandler(void) { uart_array[1]->irq_handler(); }

} // namespace librmcs::firmware::uart
