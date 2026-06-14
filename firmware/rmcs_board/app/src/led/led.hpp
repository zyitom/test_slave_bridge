#pragma once

#include <atomic>
#include <cstdint>

#include "board_app.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"
#if BOARD_LED_USE_WS2812
#include "firmware/rmcs_board/app/src/led/ws2812.hpp"
#else
#include "firmware/rmcs_board/app/src/led/gpio_led.hpp"
#endif

namespace librmcs::firmware::led {

// Board-selected LED backend. Both Ws2812 and GpioLed expose the same
// init()/set_value(r, g, b) interface, so the Led light language below is
// shared across boards regardless of the underlying LED hardware.
#if BOARD_LED_USE_WS2812
inline auto& led_backend = ws2812;
#else
inline auto& led_backend = gpio_led;
#endif

class Led {
public:
    using Lazy = utility::Lazy<Led>;

    Led() { led_backend.init(); }

    void reset() {
        uplink_full_reset_counter_.store(0, std::memory_order::relaxed);
        downlink_full_reset_counter_.store(0, std::memory_order::relaxed);
    }

    void uplink_buffer_full() {
        uplink_full_reset_counter_.store(5000, std::memory_order::relaxed);
    }

    void downlink_buffer_full() {
        downlink_full_reset_counter_.store(5000, std::memory_order::relaxed);
    }

    void update(uint32_t tick) {
        uint16_t uplink_full;
        do {
            uplink_full = uplink_full_reset_counter_.load(std::memory_order::relaxed);
            if (uplink_full == 0)
                break;
        } while (!uplink_full_reset_counter_.compare_exchange_weak(
            uplink_full, uplink_full - 1, std::memory_order::relaxed));

        uint16_t downlink_full;
        do {
            downlink_full = downlink_full_reset_counter_.load(std::memory_order::relaxed);
            if (downlink_full == 0)
                break;
        } while (!downlink_full_reset_counter_.compare_exchange_weak(
            downlink_full, downlink_full - 1, std::memory_order::relaxed));

#if BOARD_LED_USE_WS2812
        // Full-color WS2812: blend colors per state (green breathing when idle,
        // yellow/cyan when a buffer fills up).
        if (uplink_full && downlink_full) {
            if (tick & 128U)
                led_backend->set_value(255, 255, 0);
            else
                led_backend->set_value(0, 255, 255);
        } else if (uplink_full) {
            if (tick & 128U)
                led_backend->set_value(255, 255, 0);
            else
                led_backend->set_value(0, 0, 0);
        } else if (downlink_full) {
            if (tick & 128U)
                led_backend->set_value(0, 0, 0);
            else
                led_backend->set_value(0, 255, 255);
        } else {
            uint32_t brightness = (tick >> 2U) & 511U;
            if (brightness > 255U)
                brightness = 511U - brightness;
            led_backend->set_value(0, static_cast<uint8_t>(brightness), 0);
        }
#else
        // Simple 3-channel GPIO RGB LED (each color is only on/off). Keep every
        // state a single, clearly distinguishable color and never light all
        // three channels at once, so the status stays readable:
        //   green breathing = healthy/idle
        //   red blink       = uplink (board -> host) buffer full
        //   blue blink      = downlink (host -> board) buffer full
        //   red/blue alarm  = both directions congested
        const bool on = (tick & 128U) != 0;
        if (uplink_full && downlink_full) {
            led_backend->set_value(on ? 255 : 0, 0, on ? 0 : 255);
        } else if (uplink_full) {
            led_backend->set_value(on ? 255 : 0, 0, 0);
        } else if (downlink_full) {
            led_backend->set_value(0, 0, on ? 255 : 0);
        } else if (host_connected_.load(std::memory_order::relaxed)) {
            // Connected to the host and healthy: steady green.
            led_backend->set_value(0, 255, 0);
        } else {
            // No host yet: slow green blink (~1Hz) = alive, waiting for host.
            led_backend->set_value(0, (tick & 512U) ? 255 : 0, 0);
        }
#endif
    }

    void set_host_connected(bool connected) {
        host_connected_.store(connected, std::memory_order::relaxed);
    }

private:
    std::atomic<uint16_t> uplink_full_reset_counter_{0};
    std::atomic<uint16_t> downlink_full_reset_counter_{0};
    std::atomic<bool> host_connected_{false};
};

inline constinit Led::Lazy led;

} // namespace librmcs::firmware::led
