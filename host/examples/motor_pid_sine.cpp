// Motor 3519 PID position-control sine-wave test via HPM5321 CAN bridge.
//
// Controls motor ID 2 through the 一控四 protocol (CAN 2.0):
//   - Control frame:  CAN ID 0x200, motor-2 torque-current at bytes [2:3]
//   - Feedback frame: CAN ID 0x202 (0x200 + motor_id), 1 ms period
//   - Torque-current range: [-16384, 16384]  ->  [-20.5 A, 20.5 A]
//   - Angle range:          [0, 8191]        ->  [0 deg, 360 deg]
//
// Velocity is derived from consecutive position samples divided by the
// CAN hardware timestamp delta (1 us resolution), NOT from the motor's
// built-in speed estimate.  Stop with Ctrl-C.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

#include <librmcs/agent/rmcs_board_hpm5321.hpp>

namespace {

// ── Motor / CAN identifiers (3519 一控四 protocol) ──────────────────────
constexpr unsigned kMotorId = 2;
constexpr uint32_t kControlCanId = 0x200;
constexpr uint32_t kFeedbackCanId = 0x202;

// ── Pure proportional position control ──────────────────────────────────
constexpr float kKp = 8.0F;     // position error -> torque current
constexpr float kMaxCurrent = 12000.0F;  // ~15 A (75 % of full-scale)

// ── Sine trajectory ─────────────────────────────────────────────────────
constexpr float kAmplitude = 3000.0F;  // ±132 deg
constexpr float kFrequency = 2.0F;     // Hz

// ── Control loop timing ─────────────────────────────────────────────────
constexpr float kLoopHz = 1000.0F;
constexpr auto kLoopPeriod = std::chrono::microseconds{1000};

// ── Shutdown ────────────────────────────────────────────────────────────
std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }

float wrap_delta(float delta) {
    while (delta > 4096.0F) delta -= 8192.0F;
    while (delta < -4096.0F) delta += 8192.0F;
    return delta;
}

}  // namespace

class MotorPidSine : public librmcs::agent::RmcsBoardHpm5321 {
public:
    MotorPidSine()
        : librmcs::agent::RmcsBoardHpm5321{{}, {.dangerously_skip_version_checks = true}} {}

    struct Sample {
        uint16_t raw_angle = 0;
        bool valid = false;
        std::optional<uint32_t> timestamp_us;
    };

    Sample get_sample() const {
        std::lock_guard lock(mutex_);
        return sample_;
    }

    void send_control(int16_t current) {
        std::array<std::byte, 8> frame{};
        frame[2] = static_cast<std::byte>((current >> 8) & 0xFF);
        frame[3] = static_cast<std::byte>(current & 0xFF);
        start_transmit().can0_transmit({.can_id = kControlCanId, .can_data = frame,.is_fdcan = false});
    }

private:
    void can0_receive_callback(const librmcs::data::CanDataView& data) override {
        if (data.can_id != kFeedbackCanId)
            return;
        const auto b = data.can_data;
        if (b.size() < 8)
            return;

        Sample s;
        s.raw_angle = static_cast<uint16_t>(
            (std::to_integer<unsigned>(b[0]) << 8) | std::to_integer<unsigned>(b[1]));
        s.timestamp_us = data.timestamp_us;
        s.valid = true;

        std::lock_guard lock(mutex_);
        sample_ = s;
    }

    mutable std::mutex mutex_;
    Sample sample_;
};

int main() {
    std::signal(SIGINT, on_sigint);

    printf("Motor PID Sine — HPM5321 CAN bridge (hw-timestamp derived velocity)\n");
    printf("  motor ID     : %u\n", kMotorId);
    printf("  control CAN  : 0x%03X\n", kControlCanId);
    printf("  feedback CAN : 0x%03X\n", kFeedbackCanId);
    printf("  Pure-P  Kp=%.1f  Imax=%.0f\n", kKp, kMaxCurrent);
    printf("  sine amp=%.0f  freq=%.1f Hz (%.0f rpm peak)  loop=%.0f Hz\n",
           kAmplitude, kFrequency,
           kAmplitude * 2.0F * float(M_PI) * kFrequency * 60.0F / 8192.0F,
           kLoopHz);
    printf("Connecting ...\n");

    MotorPidSine agent;
    printf("Connected.  Waiting for motor feedback ...\n");

    // ── Wait for first feedback ─────────────────────────────────────────
    float sine_centre = 0.0F;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (g_running.load() && std::chrono::steady_clock::now() < deadline) {
            auto s = agent.get_sample();
            if (s.valid) {
                sine_centre = static_cast<float>(s.raw_angle);
                printf("  motor present — initial angle %u (%.1f deg)%s\n",
                       s.raw_angle, s.raw_angle * 360.0F / 8192.0F,
                       s.timestamp_us ? "" : "  (no hw timestamp!)");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (sine_centre == 0.0F) {
            fprintf(stderr, "ERROR: No motor feedback within 3 s.\n");
            return 1;
        }
    }

    // ── Phase 1: hold position while ramping current (2 s) ──────────────
    // During ramp the motor may drift; a gentle P-hold keeps it near the
    // sine centre so the sine wave doesn't start with a 180 deg step.
    printf("Phase 1 — holding position during current ramp (2 s) ...\n");
    constexpr float kHoldKp = 2.0F;
    constexpr float kRampDuration = 2.0F;
    auto ramp_start = std::chrono::steady_clock::now();
    {
        float current_limit = 0.0F;
        constexpr float kRampRate = kMaxCurrent / kRampDuration;
        auto next_loop = ramp_start;
        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();
            const float elapsed =
                std::chrono::duration<float>(now - ramp_start).count();
            if (elapsed >= kRampDuration)
                break;
            current_limit = kRampRate * elapsed;
            if (current_limit > kMaxCurrent)
                current_limit = kMaxCurrent;

            auto s = agent.get_sample();
            const float err =
                wrap_delta(sine_centre - static_cast<float>(s.raw_angle));
            float out = kHoldKp * err;
            out = std::clamp(out, -current_limit, current_limit);
            agent.send_control(static_cast<int16_t>(out));

            next_loop += kLoopPeriod;
            if (next_loop < now) next_loop = now;
            std::this_thread::sleep_until(next_loop);
        }
    }

    // Re-centre the sine wave on the motor's actual position after ramp.
    {
        auto s = agent.get_sample();
        if (s.valid)
            sine_centre = static_cast<float>(s.raw_angle);
    }
    printf("Phase 2 — starting sine (centre %.0f, %.1f deg)\n",
           sine_centre, sine_centre * 360.0F / 8192.0F);

    // ── Velocity derivation state ───────────────────────────────────────
    uint16_t prev_raw_angle = static_cast<uint16_t>(sine_centre);
    uint16_t prev_angle_vel = prev_raw_angle;
    uint32_t prev_ts_us = 0;
    bool have_prev = false;

    // ── Multi-turn tracking ─────────────────────────────────────────────
    int32_t full_turns = 0;

    uint64_t loop_count = 0;
    auto next_loop = std::chrono::steady_clock::now();
    auto last_report = next_loop;

    while (g_running.load()) {
        const auto now = std::chrono::steady_clock::now();

        // ── Trajectory ──────────────────────────────────────────────────
        const float t = static_cast<float>(loop_count) / kLoopHz;
        const float target_angle =
            sine_centre + kAmplitude * std::sin(2.0F * float(M_PI) * kFrequency * t);
        const float target_vel = kAmplitude * 2.0F * float(M_PI) * kFrequency
            * std::cos(2.0F * float(M_PI) * kFrequency * t);   // angle-units / s
        const float target_vel_rpm = target_vel * 60.0F / 8192.0F;

        // ── Feedback ────────────────────────────────────────────────────
        auto s = agent.get_sample();
        const uint16_t raw_now = s.raw_angle;

        // Multi-turn unwrap.
        int16_t delta16 = static_cast<int16_t>(raw_now - prev_raw_angle);
        if (delta16 > 4096)
            full_turns--;
        else if (delta16 < -4096)
            full_turns++;
        prev_raw_angle = raw_now;
        const float continuous_angle =
            static_cast<float>(full_turns) * 8192.0F + static_cast<float>(raw_now);

        // ── Derived velocity from hw timestamp delta ────────────────────
        float derived_vel_rpm = 0.0F;
        if (have_prev && s.timestamp_us.has_value()) {
            const uint32_t ts_now = s.timestamp_us.value();
            const int32_t ts_delta_us = static_cast<int32_t>(ts_now - prev_ts_us);
            if (ts_delta_us > 0) {
                const float dt = static_cast<float>(ts_delta_us) * 1.0e-6F;
                const float d_angle =
                    wrap_delta(static_cast<float>(raw_now) - static_cast<float>(prev_angle_vel));
                derived_vel_rpm = (d_angle / dt) * 60.0F / 8192.0F;
            }
        }
        if (s.timestamp_us.has_value()) {
            prev_ts_us = s.timestamp_us.value();
            prev_angle_vel = raw_now;
            have_prev = true;
        }

        // ── Pure P controller ──────────────────────────────────────────
        const float pos_error = wrap_delta(target_angle - continuous_angle);
        float output = kKp * pos_error;
        output = std::clamp(output, -kMaxCurrent, kMaxCurrent);

        agent.send_control(static_cast<int16_t>(output));

        // ── Timing ──────────────────────────────────────────────────────
        next_loop += kLoopPeriod;
        if (next_loop < now)
            next_loop = now;
        std::this_thread::sleep_until(next_loop);
        ++loop_count;

        // ── Report (1 Hz) ───────────────────────────────────────────────
        if (now - last_report >= std::chrono::seconds{1}) {
            last_report = now;
            printf(
                "ang %6.0f (%5.1f deg) | tgt %6.0f (%5.1f deg) | err %6.1f | "
                "vel %6.1f rpm | tgt_vel %6.1f rpm | out %6.0f | %lu Hz | turn %d\n",
                continuous_angle, continuous_angle * 360.0F / 8192.0F,
                target_angle, target_angle * 360.0F / 8192.0F,
                pos_error, derived_vel_rpm, target_vel_rpm,
                output, loop_count, full_turns);
            loop_count = 0;
        }
    }

    printf("\nZeroing motor ...\n");
    for (int i = 0; i < 5; ++i) {
        agent.send_control(0);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    printf("Stopped.\n");
    return 0;
}
