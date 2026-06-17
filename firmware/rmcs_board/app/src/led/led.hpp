#pragma once

#include <atomic>
#include <cstdint>

#include "firmware/rmcs_board/app/src/led/gpio_led.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::led {

inline auto& led_backend = gpio_led;

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

        // Simple 3-channel GPIO RGB LED (each color is only on/off). Keep every
        // state a single, clearly distinguishable color and never light all
        // three channels at once, so the status stays readable:
        //   green breathing = healthy/idle
        //   blue blink      = uplink (board -> host) buffer full
        //   red blink       = downlink (host -> board) buffer full
        //   red/blue alarm  = both directions congested
        const bool on = (tick & 128U) != 0;
        if (uplink_full && downlink_full) {
            led_backend->set_value(on ? 255 : 0, 0, on ? 0 : 255);
        } else if (uplink_full) {
            led_backend->set_value(0, 0, on ? 255 : 0);
        } else if (downlink_full) {
            led_backend->set_value(on ? 255 : 0, 0, 0);
        } else if (host_connected_.load(std::memory_order::relaxed)) {
            // Connected to the host and healthy: steady green.
            led_backend->set_value(0, 255, 0);
        } else {
            // No host yet: slow green blink (~1Hz) = alive, waiting for host.
            led_backend->set_value(0, (tick & 512U) ? 255 : 0, 0);
        }
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
