// Queue-free CAN-FD loopback latency for the HPM6E8Y bridge over either USB
// or EtherCAT. Wire CAN0 and CAN1 as one terminated bus. One frame is in
// flight at a time, so the host-clock RTT includes transport downlink, board
// CAN TX, one CAN frame on the wire, board CAN RX, and transport uplink.
//
// Run:
//   sudo ./bridge_can_loopback_latency usb [samples] [io_core] [main_core]
//   sudo env RMCS_ECAT_BACKEND=igh ./bridge_can_loopback_latency ecat enp2s0 2000 7 6

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

#include <librmcs/board/common.hpp>
#include <librmcs/board/rmcs_board_ecat_bridge.hpp>
#include <librmcs/board/rmcs_board_hpm6e8y.hpp>

namespace {

constexpr uint32_t kCanId = 0x556;
constexpr size_t kPayloadSize = 8;
constexpr uint32_t kWarmupSamples = 100;
constexpr auto kEchoTimeout = std::chrono::milliseconds{20};

uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void put_u32_le(std::byte* dst, uint32_t value) {
    dst[0] = static_cast<std::byte>(value);
    dst[1] = static_cast<std::byte>(value >> 8);
    dst[2] = static_cast<std::byte>(value >> 16);
    dst[3] = static_cast<std::byte>(value >> 24);
}

uint32_t get_u32_le(const std::byte* src) {
    return static_cast<uint32_t>(std::to_integer<uint8_t>(src[0]))
         | static_cast<uint32_t>(std::to_integer<uint8_t>(src[1])) << 8
         | static_cast<uint32_t>(std::to_integer<uint8_t>(src[2])) << 16
         | static_cast<uint32_t>(std::to_integer<uint8_t>(src[3])) << 24;
}

void configure_thread(int core, int priority, const char* name) noexcept {
    if (name)
        (void)pthread_setname_np(pthread_self(), name);

    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            perror("sched_setaffinity");
    }

    sched_param parameter{};
    parameter.sched_priority = priority;
    if (sched_setscheduler(0, SCHED_FIFO, &parameter) != 0)
        perror("sched_setscheduler");
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty())
        return 0.0;
    const size_t index =
        static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

class Receiver final
    : public librmcs::board::RmcsBoardEcatBridge::Callback
    , public librmcs::board::RmcsBoardHpm6e8y::Callback {
public:
    using Clock = std::chrono::steady_clock;

    void arm(uint32_t sequence, Clock::time_point send_time) {
        while ((state_.load(std::memory_order_acquire) & kStateMask) == kWritingState) {}
        send_time_ns_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(send_time.time_since_epoch())
                .count(),
            std::memory_order_relaxed);
        state_.store(static_cast<uint64_t>(sequence) << kStateBits, std::memory_order_release);
    }

    bool wait(uint32_t sequence, double& rtt_us) {
        const auto deadline = Clock::now() + kEchoTimeout;
        const uint64_t received_state =
            (static_cast<uint64_t>(sequence) << kStateBits) | kReceivedState;
        while (state_.load(std::memory_order_acquire) != received_state) {
            if (Clock::now() >= deadline)
                return false;
        }
        rtt_us = static_cast<double>(rtt_ns_.load(std::memory_order_relaxed)) / 1e3;
        return true;
    }

    uint64_t invalid() const { return invalid_.load(std::memory_order_relaxed); }
    uint64_t unexpected() const { return unexpected_.load(std::memory_order_relaxed); }

private:
    void can1_receive_callback(const librmcs::data::CanDataView& data) override {
        const auto receive_time = Clock::now();
        if (data.can_id != kCanId || !data.is_fdcan || data.is_extended_can_id
            || data.is_remote_transmission || data.can_data.size() != kPayloadSize) {
            invalid_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const uint32_t sequence = get_u32_le(data.can_data.data());
        if (get_u32_le(data.can_data.data() + 4) != mix(sequence)) {
            invalid_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        uint64_t expected_state = static_cast<uint64_t>(sequence) << kStateBits;
        if (!state_.compare_exchange_strong(
                expected_state, expected_state | kWritingState, std::memory_order_acquire,
                std::memory_order_relaxed)) {
            unexpected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const int64_t receive_time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(receive_time.time_since_epoch())
                .count();
        rtt_ns_.store(
            receive_time_ns - send_time_ns_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        state_.store(
            (static_cast<uint64_t>(sequence) << kStateBits) | kReceivedState,
            std::memory_order_release);
    }

    void can0_receive_callback(const librmcs::data::CanDataView&) override {
        unexpected_.fetch_add(1, std::memory_order_relaxed);
    }
    void can2_receive_callback(const librmcs::data::CanDataView&) override {
        unexpected_.fetch_add(1, std::memory_order_relaxed);
    }
    void can3_receive_callback(const librmcs::data::CanDataView&) override {
        unexpected_.fetch_add(1, std::memory_order_relaxed);
    }

    static constexpr unsigned kStateBits = 2;
    static constexpr uint64_t kStateMask = (1U << kStateBits) - 1U;
    static constexpr uint64_t kWritingState = 1;
    static constexpr uint64_t kReceivedState = 2;

    std::atomic<uint64_t> state_{kReceivedState};
    std::atomic<int64_t> send_time_ns_{0};
    std::atomic<int64_t> rtt_ns_{0};
    std::atomic<uint64_t> invalid_{0};
    std::atomic<uint64_t> unexpected_{0};
};

template <typename Board>
int run_latency(Board& board, Receiver& receiver, uint32_t samples, std::string_view transport) {
    std::vector<double> rtts_us;
    rtts_us.reserve(samples);
    uint32_t timeouts = 0;

    printf(
        "%.*s: %u measured CAN-FD round trips after %u warm-up samples\n",
        static_cast<int>(transport.size()), transport.data(), samples, kWarmupSamples);

    for (uint32_t sequence = 0; sequence < samples + kWarmupSamples; ++sequence) {
        std::byte payload[kPayloadSize];
        put_u32_le(payload, sequence);
        put_u32_le(payload + 4, mix(sequence));

        receiver.arm(sequence, Receiver::Clock::now());
        board.start_transmit().can0_transmit(
            {.can_id = kCanId, .can_data = payload, .is_fdcan = true});

        double rtt_us = 0.0;
        if (!receiver.wait(sequence, rtt_us)) {
            if (sequence >= kWarmupSamples)
                ++timeouts;
            continue;
        }
        if (sequence >= kWarmupSamples)
            rtts_us.push_back(rtt_us);
    }

    if (rtts_us.empty()) {
        fprintf(stderr, "no valid loopback frames received\n");
        return 2;
    }

    std::sort(rtts_us.begin(), rtts_us.end());
    double sum = 0.0;
    for (const double value : rtts_us)
        sum += value;

    printf(
        "samples=%zu timeout=%u invalid=%llu unexpected=%llu\n", rtts_us.size(), timeouts,
        static_cast<unsigned long long>(receiver.invalid()),
        static_cast<unsigned long long>(receiver.unexpected()));
    printf(
        "rtt us: min %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  avg %.1f  max %.1f\n",
        rtts_us.front(), percentile(rtts_us, 0.50), percentile(rtts_us, 0.90),
        percentile(rtts_us, 0.99), percentile(rtts_us, 0.999),
        sum / static_cast<double>(rtts_us.size()), rtts_us.back());

    const bool clean = rtts_us.size() == samples && timeouts == 0 && receiver.invalid() == 0
                    && receiver.unexpected() == 0;
    printf("result: %s\n", clean ? "PASS" : "CHECK counters above");
    return clean ? 0 : 2;
}

int parse_int(const char* value, int fallback) {
    return value ? std::atoi(value) : fallback;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(
            stderr,
            "usage: %s usb [samples] [io_core] [main_core]\n"
            "       %s ecat <interface> [samples] [io_core] [main_core]\n",
            argv[0], argv[0]);
        return 1;
    }

    const std::string_view mode = argv[1];
    const bool use_ecat = mode == "ecat";
    if (!use_ecat && mode != "usb") {
        fprintf(stderr, "transport must be usb or ecat\n");
        return 1;
    }
    if (use_ecat && argc < 3) {
        fprintf(stderr, "ecat mode requires an interface hint\n");
        return 1;
    }

    const int argument_base = use_ecat ? 3 : 2;
    const int sample_value = parse_int(argc > argument_base ? argv[argument_base] : nullptr, 2000);
    const int io_core =
        parse_int(argc > argument_base + 1 ? argv[argument_base + 1] : nullptr, -1);
    const int main_core =
        parse_int(argc > argument_base + 2 ? argv[argument_base + 2] : nullptr, -1);
    if (sample_value <= 0) {
        fprintf(stderr, "samples must be positive\n");
        return 1;
    }

    auto options = librmcs::board::bind_advanced_options([io_core]() noexcept {
        configure_thread(io_core, 80, "bridge-lat-io");
    });

    Receiver receiver;
    try {
        if (use_ecat) {
            librmcs::board::RmcsBoardEcatBridge board{argv[2], receiver, options};
            // Configure the busy-waiting sender only after the SDK has created
            // its keepalive thread, so that thread does not inherit this CPU and
            // SCHED_FIFO priority and starve until the board's 1 s lease expires.
            configure_thread(main_core, 70, "bridge-lat-main");
            return run_latency(
                board, receiver, static_cast<uint32_t>(sample_value), "EtherCAT");
        }

        librmcs::board::RmcsBoardHpm6e8y board{receiver, {}, options};
        configure_thread(main_core, 70, "bridge-lat-main");
        return run_latency(board, receiver, static_cast<uint32_t>(sample_value), "USB");
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
