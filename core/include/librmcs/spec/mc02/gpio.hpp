#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

#include <librmcs/spec/gpio.hpp>

namespace librmcs::spec::mc02 {

namespace internal {
class GpioDescriptors;
}

// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class GpioDescriptor : public spec::GpioDescriptor {
    friend internal::GpioDescriptors;
    constexpr GpioDescriptor(uint8_t channel_index, GpioCapability capability_mask)
        : spec::GpioDescriptor(channel_index, capability_mask) {}

public:
    GpioDescriptor(const GpioDescriptor&) = delete;
    GpioDescriptor& operator=(const GpioDescriptor&) = delete;
    GpioDescriptor(GpioDescriptor&&) = delete;
    GpioDescriptor& operator=(GpioDescriptor&&) = delete;

    [[nodiscard]] constexpr bool operator==(const GpioDescriptor& other) const noexcept {
        return channel_index == other.channel_index;
    }
};

namespace internal {
class GpioDescriptors {
    // mc02 exposes four PWM-capable output pins. Unlike c_board they are
    // write-only, so they advertise digital/analog write without any read
    // capability.
    static constexpr GpioCapability kPwmWriteCapabilities =
        GpioCapability::kDigitalWrite | GpioCapability::kAnalogWrite;

    static constexpr GpioDescriptor kArray[]{
        {0, kPwmWriteCapabilities}, // TIM2 CH1 (PA0)
        {1, kPwmWriteCapabilities}, // TIM2 CH3 (PA2)
        {2, kPwmWriteCapabilities}, // TIM1 CH1 (PE9)
        {3, kPwmWriteCapabilities}  // TIM1 CH3 (PE13)
    };
    static_assert(channel_indices_match_indices(kArray));

public:
    constexpr GpioDescriptors() = default;

    static constexpr std::size_t size() noexcept { return std::size(kArray); }

    static constexpr const GpioDescriptor& operator[](std::size_t channel_index) noexcept {
        return kArray[channel_index];
    }

    static constexpr const GpioDescriptor* begin() noexcept { return std::begin(kArray); }

    static constexpr const GpioDescriptor* end() noexcept { return std::end(kArray); }

    static constexpr const GpioDescriptor& kPwm1 = kArray[0];
    static constexpr const GpioDescriptor& kPwm2 = kArray[1];
    static constexpr const GpioDescriptor& kPwm3 = kArray[2];
    static constexpr const GpioDescriptor& kPwm4 = kArray[3];
};
} // namespace internal

inline constexpr internal::GpioDescriptors kGpioDescriptors{};

} // namespace librmcs::spec::mc02
