#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include <librmcs/agent/rmcs_board_hpm5321.hpp>

// Receive-only monitor for the HPM5321 bridge: it SENDS NOTHING. It just counts
// incoming CAN0 + UART0 frames and measures their real-time inter-arrival
// intervals.
//
// Receiving is fully event-driven and best-effort: every IN bulk transfer that
// completes immediately fires the agent callback on the libusb event thread (and
// the transfer is re-armed at once). There is no send loop and no 1 kHz gating
// here -- whatever the board pushes is processed as soon as it arrives. The 1 s
// cadence below is only for printing the summary, not for measuring.
//
// Run with LIBRMCS_USB_STATS=1 to additionally see the USB bulk-transfer cadence
// (transfers/s, bytes/s, inferred packets, and interval min/avg/max).

namespace {

std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }

// Per-direction counters + inter-arrival interval, updated in the receive
// callbacks (libusb event thread) and drained by the reporting thread.
struct RxStat {
    std::mutex mutex;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t interval_min_ns = UINT64_MAX;
    uint64_t interval_max_ns = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::time_point::min();

    void record(std::size_t size) {
        const auto now = std::chrono::steady_clock::now();
        const std::scoped_lock guard{mutex};
        frames += 1;
        bytes += size;
        if (last != std::chrono::steady_clock::time_point::min()) {
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
            if (static_cast<uint64_t>(ns) < interval_min_ns)
                interval_min_ns = static_cast<uint64_t>(ns);
            if (static_cast<uint64_t>(ns) > interval_max_ns)
                interval_max_ns = static_cast<uint64_t>(ns);
        }
        last = now;
    }

    struct Snapshot {
        uint64_t frames, bytes, min_ns, max_ns;
    };
    Snapshot take() {
        const std::scoped_lock guard{mutex};
        const Snapshot snapshot{frames, bytes, interval_min_ns, interval_max_ns};
        frames = 0;
        bytes = 0;
        interval_min_ns = UINT64_MAX;
        interval_max_ns = 0;
        // Keep "last" so the interval stays continuous across reporting windows.
        return snapshot;
    }
};

void print_dir(const char* name, const RxStat::Snapshot& s, double dt) {
    const double avg_us = s.frames ? dt / static_cast<double>(s.frames) * 1e6 : 0.0;
    const double min_us = s.min_ns == UINT64_MAX ? 0.0 : static_cast<double>(s.min_ns) / 1e3;
    const double max_us = static_cast<double>(s.max_ns) / 1e3;
    printf(
        "%-9s %8.0f frame/s %9.0f B/s | interval us min/avg/max = %.0f/%.0f/%.0f\n", name,
        static_cast<double>(s.frames) / dt, static_cast<double>(s.bytes) / dt, min_us, avg_us,
        max_us);
}

} // namespace

class RxMonitor : public librmcs::agent::RmcsBoardHpm5321 {
public:
    RxMonitor()
        : librmcs::agent::RmcsBoardHpm5321{{}, {.dangerously_skip_version_checks = true}} {}

    RxStat can0, uart0;

private:
    void can0_receive_callback(const librmcs::data::CanDataView& data) override {
        can0.record(data.can_data.size());
    }
    void uart0_receive_callback(const librmcs::data::UartDataView& data) override {
        uart0.record(data.uart_data.size());
    }
};

int main() {
    std::signal(SIGINT, on_sigint);

    printf("RX monitor - connecting to HPM5321 bridge (receive only)...\n");
    RxMonitor agent;
    printf("Connected. Listening; nothing is sent. Ctrl-C to stop.\n");

    auto last_report = std::chrono::steady_clock::now();
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::seconds{1})
            continue;
        const double dt = std::chrono::duration<double>(now - last_report).count();
        last_report = now;
        print_dir("CAN0 RX", agent.can0.take(), dt);
        print_dir("UART0 RX", agent.uart0.take(), dt);
    }

    printf("\nStopped.\n");
    return 0;
}
