#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <ratio>

#include <stm32h723xx.h>

namespace librmcs::firmware::timer {

constexpr uint32_t kSystemFrequency = 550'000'000;
using SysFreqDuration = std::chrono::duration<uint32_t, std::ratio<1, kSystemFrequency>>;

inline void delay_basic(SysFreqDuration d) {
    if (!d.count()) [[unlikely]]
        return;

    const uint32_t start = DWT->CYCCNT;
    const uint32_t end = start + d.count();

    if (end < start) {
        while (DWT->CYCCNT >= start)
            ;
    }
    while (DWT->CYCCNT < end)
        ;
}

template <std::integral Rep, typename Period>
inline void delay(std::chrono::duration<Rep, Period> d) {
    if constexpr (std::is_signed_v<Rep>) {
        if (d.count() < 0) [[unlikely]]
            return;
    }

    using DurationT = std::chrono::duration<uint32_t, Period>;
    auto casted = DurationT{static_cast<uint32_t>(d.count())};
    constexpr auto kMax = std::chrono::floor<DurationT>(SysFreqDuration::max());
    static_assert(kMax.count() > 0, "Unit too large; choose a smaller unit");

    while (casted > kMax) {
        casted -= kMax;
        delay_basic(SysFreqDuration::max());
    }
    delay_basic(std::chrono::round<SysFreqDuration>(casted));
}

} // namespace librmcs::firmware::timer
