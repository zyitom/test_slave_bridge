#include "core/src/utility/assert.hpp"

#include <source_location>

#include "firmware/ch32_board/app/src/led/led.hpp"
#include "firmware/ch32_board/app/src/utility/interrupt_lock.hpp"

namespace librmcs::core::utility {

const char* volatile assert_file = nullptr;
volatile unsigned int assert_line = 0;
const char* volatile assert_function = nullptr;

namespace {
inline void force_led_red() noexcept {
    // No-op on ch32_board (the Led is a stub, see led.hpp). Kept structurally
    // identical to mc02 so this file stays a near-verbatim copy; try_get()
    // returns nullptr until the LED is constructed.
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
