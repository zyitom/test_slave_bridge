#include "firmware/mc02/app/src/gpio/gpio.hpp"

#include <cstdint>

#include <gpio.h>
#include <main.h>

#include "firmware/mc02/app/src/spi/bmi088/accel.hpp"
#include "firmware/mc02/app/src/spi/bmi088/gyro.hpp"
#include "firmware/mc02/app/src/timer/timer.hpp"

namespace librmcs::firmware::gpio {

extern "C" void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == INT1_ACC_Pin) {
#ifdef LIBRMCS_APP_IMU_ENABLE
        // Capture the data-ready edge time as close to the interrupt as possible;
        // the SPI read that fetches the sample happens later in the main loop.
        const uint32_t capture_timestamp_quarter_us =
            timer::timer->timepoint().time_since_epoch().count();
        if (spi::bmi088::accelerometer)
            spi::bmi088::accelerometer->data_ready_callback(capture_timestamp_quarter_us);
#endif
    } else if (gpio_pin == INT1_GYRO_Pin) {
#ifdef LIBRMCS_APP_IMU_ENABLE
        const uint32_t capture_timestamp_quarter_us =
            timer::timer->timepoint().time_since_epoch().count();
        if (spi::bmi088::gyroscope)
            spi::bmi088::gyroscope->data_ready_callback(capture_timestamp_quarter_us);
#endif
    } else {
        gpio::gpio->handle_input_edge_interrupt(gpio_pin);
    }
}

// Channel edge interrupts. PA0/PA2/PE9 use dedicated EXTI lines whose handlers
// CubeMX does not generate (the pins boot as PWM), so they are provided here
// app-side and stay immune to .ioc regeneration. PE13 shares the CubeMX-owned
// EXTI15_10 handler, which forwards GPIO_PIN_13 through a USER CODE section.
extern "C" void EXTI0_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0); }
extern "C" void EXTI2_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_2); }
extern "C" void EXTI9_5_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_9); }

} // namespace librmcs::firmware::gpio
