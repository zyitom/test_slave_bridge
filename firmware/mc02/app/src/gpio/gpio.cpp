#include <cstdint>

#include <gpio.h>
#include <main.h>

#include "firmware/mc02/app/src/spi/bmi088/accel.hpp"
#include "firmware/mc02/app/src/spi/bmi088/gyro.hpp"

namespace librmcs::firmware::gpio {

extern "C" void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == INT1_ACC_Pin) {
        if (spi::bmi088::accelerometer)
            spi::bmi088::accelerometer->data_ready_callback();
    } else if (gpio_pin == INT1_GYRO_Pin) {
        if (spi::bmi088::gyroscope)
            spi::bmi088::gyroscope->data_ready_callback();
    }
}

} // namespace librmcs::firmware::gpio
