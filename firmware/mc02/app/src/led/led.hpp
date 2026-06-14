#pragma once

#include <atomic>
#include <cstdint>

#include <spi.h>

#include "firmware/mc02/app/src/utility/lazy.hpp"

namespace librmcs::firmware::led {

class Led {
public:
    Led() { reset(); }

    void reset() {
        uplink_full_reset_counter_.store(0, std::memory_order::relaxed);
        downlink_full_reset_counter_.store(0, std::memory_order::relaxed);
        user_controlling_.store(false, std::memory_order::relaxed);
    }

    void uplink_buffer_full() {
        uplink_full_reset_counter_.store(5000, std::memory_order::relaxed);
    }

    void downlink_buffer_full() {
        downlink_full_reset_counter_.store(5000, std::memory_order::relaxed);
    }

    void update(uint32_t tick) {
        if (user_controlling_.load(std::memory_order::relaxed))
            return;

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

        if (uplink_full && downlink_full) {
            if (tick & 128)
                set_value(255, 255, 0);
            else
                set_value(0, 255, 255);
        } else if (uplink_full) {
            if (tick & 128)
                set_value(255, 255, 0);
            else
                set_value(0, 0, 0);
        } else if (downlink_full) {
            if (tick & 128)
                set_value(0, 0, 0);
            else
                set_value(0, 255, 255);
        } else {
            auto brightness = (tick >> 2) & 511;
            if (brightness > 255)
                brightness = 511 - brightness;
            set_value(0, static_cast<uint8_t>(brightness), 0);
        }
    }

    // Non-static to ensure instantiation
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void set_value(uint8_t red, uint8_t green, uint8_t blue) {
        static uint32_t last_color = 0;
        const uint32_t color = (static_cast<uint32_t>(red) << 16)
                             | (static_cast<uint32_t>(green) << 8)
                             | blue;
        if (color == last_color)
            return;
        last_color = color;

        static uint8_t txbuf[124] = {0};
        const uint8_t ws2812_high = 0xf0;
        const uint8_t ws2812_low = 0xC0;
        for (int i = 0; i < 8; i++) {
            txbuf[7 - i] = (((green >> i) & 0x01) ? ws2812_high : ws2812_low) >> 1;
            txbuf[15 - i] = (((red >> i) & 0x01) ? ws2812_high : ws2812_low) >> 1;
            txbuf[23 - i] = (((blue >> i) & 0x01) ? ws2812_high : ws2812_low) >> 1;
        }
        HAL_SPI_Transmit(&hspi6, txbuf, 124, 100);
    }

private:
    std::atomic<bool> user_controlling_;
    std::atomic<uint16_t> uplink_full_reset_counter_, downlink_full_reset_counter_;
};

inline constinit utility::Lazy<Led> led;

} // namespace librmcs::firmware::led
