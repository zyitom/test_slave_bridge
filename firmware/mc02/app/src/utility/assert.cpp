#include "core/src/utility/assert.hpp"

#include <source_location>

#include <main.h>

#include "firmware/mc02/app/src/led/led.hpp"
#include "firmware/mc02/app/src/utility/interrupt_lock.hpp"

namespace librmcs::core::utility {

const char* volatile assert_file = nullptr;
volatile unsigned int assert_line = 0;
const char* volatile assert_function = nullptr;

namespace {
inline void force_led_red() noexcept {
    // mc02's status LED is a WS2812 driven over SPI6 (not GPIO bit-bang like
    // c_board), so the panic indicator goes through the Led object. try_get()
    // returns nullptr when the LED has not been constructed yet (very early
    // panic), in which case the indicator is skipped. set_value() polls SPI6 flags
    // and completes even with interrupts disabled here.
    if (auto* led = firmware::led::led.try_get())
        led->set_value(255, 0, 0);
}
} // namespace

[[noreturn]] void assert_func(const std::source_location& location) {
    firmware::utility::InterruptMutex::lock();

    assert_file = location.file_name();
    assert_line = location.line();
    assert_function = location.function_name();

    force_led_red();

    __builtin_trap();
}

} // namespace librmcs::core::utility
