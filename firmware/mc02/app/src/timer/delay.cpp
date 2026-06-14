#include "firmware/mc02/app/src/timer/delay.hpp"

#include <stm32h7xx_hal.h>

#include "firmware/mc02/app/src/led/led.hpp"

namespace librmcs::firmware::timer {

// The STM32 DWT (Data Watchpoint and Trace) unit is used to rewrite the HAL_Delay
// function to ensure that it works when interrupts are disabled, while significantly
// improving accuracy.
extern "C" void HAL_Delay(uint32_t ms) {
    delay(std::chrono::milliseconds(ms));
}

// Hack this useless function to perform regular low-priority tasks, eliminating the
// need for a dedicated timer peripheral.
extern "C" void HAL_IncTick() {
    const uint32_t tick = uwTick + 1;
    uwTick = tick;
    led::led->update(tick);
}

} // namespace librmcs::firmware::timer
