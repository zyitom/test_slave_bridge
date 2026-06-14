#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include <librmcs/agent/c_board.hpp>

// TIM1/TIM8: 168MHz / 56 / 60000 = 50 Hz (period = 20ms)
// Standard servo pulse range: 1000-2000 us

namespace {

constexpr uint32_t k_servo_min_us = 500;
constexpr uint32_t k_servo_max_us = 2500;
constexpr uint32_t k_servo_center_us = 1500;
constexpr uint32_t k_pwm_period_us = 20000;

uint16_t pulse_us_to_duty16(uint32_t pulse_us) {
    return static_cast<uint16_t>(pulse_us * 65535u / k_pwm_period_us);
}

uint32_t angle_to_pulse_us(int deg) {
    if (deg < 0)
        deg = 0;
    if (deg > 180)
        deg = 180;
    return k_servo_min_us
        + (k_servo_max_us - k_servo_min_us) * static_cast<uint32_t>(deg) / 180;
}

const librmcs::spec::c_board::GpioDescriptor* gpio_from_channel(int ch) {
    using namespace librmcs::spec::c_board;
    switch (ch) {
    case 1: return &kGpioDescriptors.kPwm1;
    case 2: return &kGpioDescriptors.kPwm2;
    case 3: return &kGpioDescriptors.kPwm3;
    case 4: return &kGpioDescriptors.kPwm4;
    case 5: return &kGpioDescriptors.kPwm5;
    case 6: return &kGpioDescriptors.kPwm6;
    case 7: return &kGpioDescriptors.kPwm7;
    default: return nullptr;
    }
}

} // namespace

class ServoTester : public librmcs::agent::CBoard {
public:
    ServoTester()
        : librmcs::agent::CBoard{{}, {.dangerously_skip_version_checks = true}} {}

    void set_pulse(int ch, uint32_t pulse_us) {
        const auto* gpio = gpio_from_channel(ch);
        if (!gpio) {
            printf("Invalid channel %d (valid: 1-7)\n", ch);
            return;
        }
        if (pulse_us < k_servo_min_us || pulse_us > k_servo_max_us)
            printf("Warning: %u us is outside normal servo range (%u-%u us)\n",
                pulse_us, k_servo_min_us, k_servo_max_us);

        start_transmit().gpio_analog_write(*gpio, {.value = pulse_us_to_duty16(pulse_us)});
        printf("CH%d -> %u us\n", ch, pulse_us);
    }

    void set_angle(int ch, int deg) {
        const auto* gpio = gpio_from_channel(ch);
        if (!gpio) {
            printf("Invalid channel %d (valid: 1-7)\n", ch);
            return;
        }
        const uint32_t pulse_us = angle_to_pulse_us(deg);
        start_transmit().gpio_analog_write(*gpio, {.value = pulse_us_to_duty16(pulse_us)});
        printf("CH%d -> %d deg (%u us)\n", ch, deg, pulse_us);
    }

    void center_all() {
        const uint16_t duty = pulse_us_to_duty16(k_servo_center_us);
        using namespace librmcs::spec::c_board;
        start_transmit()
            .gpio_analog_write(kGpioDescriptors.kPwm1, {.value = duty})
            .gpio_analog_write(kGpioDescriptors.kPwm2, {.value = duty})
            .gpio_analog_write(kGpioDescriptors.kPwm3, {.value = duty})
            .gpio_analog_write(kGpioDescriptors.kPwm4, {.value = duty})
            .gpio_analog_write(kGpioDescriptors.kPwm5, {.value = duty})
            .gpio_analog_write(kGpioDescriptors.kPwm6, {.value = duty})
            .gpio_analog_write(kGpioDescriptors.kPwm7, {.value = duty});
        printf("All channels -> center (%u us)\n", k_servo_center_us);
    }

    void sweep(int ch) {
        const auto* gpio = gpio_from_channel(ch);
        if (!gpio) {
            printf("Invalid channel %d (valid: 1-7)\n", ch);
            return;
        }
        printf("Sweeping CH%d (0 -> 180 -> 0 deg)...\n", ch);
        for (int deg = 0; deg <= 180; deg += 3) {
            const uint32_t pulse_us = angle_to_pulse_us(deg);
            start_transmit().gpio_analog_write(*gpio, {.value = pulse_us_to_duty16(pulse_us)});
            printf("\r  %3d deg (%u us)   ", deg, pulse_us);
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        for (int deg = 180; deg >= 0; deg -= 3) {
            const uint32_t pulse_us = angle_to_pulse_us(deg);
            start_transmit().gpio_analog_write(*gpio, {.value = pulse_us_to_duty16(pulse_us)});
            printf("\r  %3d deg (%u us)   ", deg, pulse_us);
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        printf("\nDone.\n");
    }
};

static void print_help() {
    printf(
        "\nCommands:\n"
        "  set <ch> <us>      Set channel to pulse width in us   e.g. set 1 1500\n"
        "  angle <ch> <deg>   Set channel by angle (0-180 deg)   e.g. angle 2 90\n"
        "  sweep <ch>         Sweep channel 0->180->0 deg        e.g. sweep 3\n"
        "  center             Center all 7 channels (1500 us)\n"
        "  help               Show this message\n"
        "  quit               Exit\n"
        "\nChannels: 1-7  |  Pulse range: %u-%u us  |  Frequency: 50 Hz\n\n",
        k_servo_min_us, k_servo_max_us);
}

int main() {
    printf("Servo tester - connecting to c_board...\n");

    ServoTester tester;

    printf("Connected. Type 'help' for commands.\n");
    tester.center_all();

    char line[128];
    while (true) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;

        char cmd[32]{};
        int arg1 = 0;
        int arg2 = 0;
        const int n = sscanf(line, "%31s %d %d", cmd, &arg1, &arg2);
        if (n <= 0)
            continue;

        try {
            if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0
                || strcmp(cmd, "q") == 0) {
                break;
            } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
                print_help();
            } else if (strcmp(cmd, "center") == 0) {
                tester.center_all();
            } else if (strcmp(cmd, "set") == 0 && n >= 3) {
                tester.set_pulse(arg1, static_cast<uint32_t>(arg2));
            } else if (strcmp(cmd, "angle") == 0 && n >= 3) {
                tester.set_angle(arg1, arg2);
            } else if (strcmp(cmd, "sweep") == 0 && n >= 2) {
                tester.sweep(arg1);
            } else {
                printf("Unknown command or missing arguments. Type 'help' for usage.\n");
            }
        } catch (const std::exception& e) {
            printf("Error: %s\n", e.what());
        }
    }

    return 0;
}
