#pragma once

#include <cstddef>
#include <cstdint>

#include <gpio.h>
#include <main.h>
#include <tim.h>

#include "core/include/librmcs/data/datas.hpp"
#include "core/include/librmcs/spec/mc02/gpio.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/mc02/app/src/utility/lazy.hpp"

namespace librmcs::firmware::gpio {

class Gpio : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Gpio>;

    Gpio() {
        core::utility::assert_always(HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1) == HAL_OK);
        core::utility::assert_always(HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3) == HAL_OK);
        core::utility::assert_always(HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) == HAL_OK);
        core::utility::assert_always(HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3) == HAL_OK);

        for (const auto& gpio : spec::mc02::kGpioDescriptors)
            set_pwm_compare(gpio.channel_index, 0);
    }

    void handle_digital_write(uint8_t channel_index, const data::GpioDigitalDataView& data) {
        set_pwm_compare(channel_index, data.high ? kPwmCounterPeriod : 0);
    }

    void handle_analog_write(uint8_t channel_index, const data::GpioAnalogDataView& data) {
        set_pwm_compare(channel_index, duty16_to_pwm_compare(data.value));
    }

private:
    struct ChannelHardware {
        volatile uint32_t* compare_register;
    };

    // TIM1/TIM2 clock: 275 MHz, Prescaler=274, Period=19999 -> 50 Hz
    static constexpr uint32_t kPwmCounterPeriod = 20000;

    static constexpr std::size_t kChannelCount = std::size(spec::mc02::kGpioDescriptors);

    void set_pwm_compare(uint8_t channel_index, uint32_t compare) {
        const auto index = static_cast<std::size_t>(channel_index);
        core::utility::assert_debug(index < kChannelCount);
        *channel_hardware_[index].compare_register = compare;
    }

    static uint32_t duty16_to_pwm_compare(uint16_t duty) {
        return ((static_cast<uint32_t>(duty) * kPwmCounterPeriod) + 32767U) / 65535U;
    }

    const ChannelHardware channel_hardware_[kChannelCount]{
        {.compare_register = &htim2.Instance->CCR1}, // TIM2 CH1 (PA0)
        {.compare_register = &htim2.Instance->CCR3}, // TIM2 CH3 (PA2)
        {.compare_register = &htim1.Instance->CCR1}, // TIM1 CH1 (PE9)
        {.compare_register = &htim1.Instance->CCR3}, // TIM1 CH3 (PE13)
    };
};

inline constinit Gpio::Lazy gpio;

} // namespace librmcs::firmware::gpio
