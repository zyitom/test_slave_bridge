#pragma once

#include <cstdint>

#include "board_app.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::led {

// Plain GPIO RGB LED backend, used by boards whose LED is driven by simply
// pulling a pad high (active-high) instead of a WS2812 serial protocol. It
// exposes the same interface as Ws2812 so the shared Led driver is agnostic to
// the underlying hardware. Without PWM the channels can only be on or off, so
// the per-channel value is thresholded at its midpoint, turning the WS2812
// brightness ramps into blinks while keeping the blink-based light language.
class GpioLed : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<GpioLed>;

    static constexpr uint8_t kOnThreshold = 128;

    GpioLed() { board::init_led_pins(); }

    bool set_value(uint8_t red, uint8_t green, uint8_t blue) {
        board::kLedRedPin.set_active(red >= kOnThreshold);
        board::kLedGreenPin.set_active(green >= kOnThreshold);
        board::kLedBluePin.set_active(blue >= kOnThreshold);
        return true;
    }
};

inline constinit GpioLed::Lazy gpio_led;

} // namespace librmcs::firmware::led
