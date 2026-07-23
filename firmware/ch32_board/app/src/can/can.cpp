#include "firmware/ch32_board/app/src/can/can.hpp"

namespace librmcs::firmware::can {

// Thin ISR shims routing each RX0 interrupt to its array element's irq_handler().
// Plain __attribute__((interrupt)) so GCC emits mret (WCH's "WCH-Interrupt-fast"
// argument is dropped by mainline GCC).

extern "C" void CAN1_RX0_IRQHandler(void) __attribute__((interrupt()));
extern "C" void CAN1_RX0_IRQHandler(void) { can_array[0]->irq_handler(); }

extern "C" void CAN2_RX0_IRQHandler(void) __attribute__((interrupt()));
extern "C" void CAN2_RX0_IRQHandler(void) { can_array[1]->irq_handler(); }

} // namespace librmcs::firmware::can
