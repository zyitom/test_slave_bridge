#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sched.h>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>

// CPU core to pin the event thread to. Set to -1 to disable.
static constexpr int kRtCpu = 7;

#include <librmcs/agent/c_board.hpp>
#include <librmcs/agent/rmcs_board_hpm5321.hpp>

// Receive-only monitor. Auto-detects CBoard or RmcsBoardHpm5321.
// Run as root or: sudo chrt -f 80 ./rx_monitor

namespace {

std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false, std::memory_order_relaxed); }

// --- Channels -----------------------------------------------------------------
enum Channel : uint8_t { kCan0, kCan1, kCan2, kUart0, kUart1, kUart2, kDbus, kChannelCount };
static constexpr const char* kChannelName[kChannelCount] =
    {"CAN0", "CAN1", "CAN2", "UART0", "UART1", "UART2", "DBUS"};

// --- Lock-free SPSC log queue -------------------------------------------------
struct LogEntry {
    uint64_t timestamp_ns;
    uint64_t delta_ns;
    uint32_t size;
    uint32_t hw_timestamp_us;  // CAN hardware timestamp (us)
    bool has_hw_ts;
    Channel channel;
};

constexpr size_t kLogQueueSize = 1 << 14;
constexpr size_t kLogQueueMask = kLogQueueSize - 1;

struct LogQueue {
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    LogEntry entries[kLogQueueSize];

    void push(LogEntry entry) {
        const uint64_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= kLogQueueSize)
            return;
        entries[h & kLogQueueMask] = entry;
        head.store(h + 1, std::memory_order_release);
    }

    bool pop(LogEntry& out) {
        const uint64_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire))
            return false;
        out = entries[t & kLogQueueMask];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
} g_log_queue;

std::chrono::steady_clock::time_point g_start;

// --- Per-channel stats --------------------------------------------------------
struct RxStat {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> interval_min_ns{UINT64_MAX};
    std::atomic<uint64_t> interval_max_ns{0};
    std::atomic<uint64_t> interval_sum_ns{0};
    std::chrono::steady_clock::time_point last{std::chrono::steady_clock::time_point::min()};

    void record(Channel ch, std::size_t size, uint32_t hw_ts = 0, bool has_hw_ts = false) {
        const auto now = std::chrono::steady_clock::now();
        frames.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(size, std::memory_order_relaxed);

        uint64_t delta_ns = 0;
        if (last != std::chrono::steady_clock::time_point::min()) {
            delta_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
            interval_sum_ns.fetch_add(delta_ns, std::memory_order_relaxed);
            uint64_t cur = interval_min_ns.load(std::memory_order_relaxed);
            while (delta_ns < cur
                   && !interval_min_ns.compare_exchange_weak(
                       cur, delta_ns, std::memory_order_relaxed, std::memory_order_relaxed))
                ;
            cur = interval_max_ns.load(std::memory_order_relaxed);
            while (delta_ns > cur
                   && !interval_max_ns.compare_exchange_weak(
                       cur, delta_ns, std::memory_order_relaxed, std::memory_order_relaxed))
                ;
        }
        last = now;

        const uint64_t ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_start).count());
        g_log_queue.push({.timestamp_ns = ts, .delta_ns = delta_ns,
                          .size = static_cast<uint32_t>(size),
                          .hw_timestamp_us = hw_ts, .has_hw_ts = has_hw_ts,
                          .channel = ch});
    }

    struct Snapshot { uint64_t frames, bytes, min_ns, max_ns, sum_ns; };
    Snapshot take() {
        return {
            .frames  = frames.exchange(0, std::memory_order_relaxed),
            .bytes   = bytes.exchange(0, std::memory_order_relaxed),
            .min_ns  = interval_min_ns.exchange(UINT64_MAX, std::memory_order_relaxed),
            .max_ns  = interval_max_ns.exchange(0, std::memory_order_relaxed),
            .sum_ns  = interval_sum_ns.exchange(0, std::memory_order_relaxed),
        };
    }
};

RxStat g_stats[kChannelCount];

void print_summary(Channel ch, const RxStat::Snapshot& s, double dt) {
    if (s.frames == 0)
        return;
    const double avg_us = s.frames > 1
        ? static_cast<double>(s.sum_ns) / static_cast<double>(s.frames - 1) / 1e3 : 0.0;
    const double min_us = s.min_ns == UINT64_MAX ? 0.0 : static_cast<double>(s.min_ns) / 1e3;
    const double max_us = static_cast<double>(s.max_ns) / 1e3;
    printf("  %-5s  %7.0f frame/s  %8.0f B/s  |  interval us  min/avg/max = %6.1f/%6.1f/%6.1f\n",
        kChannelName[ch], static_cast<double>(s.frames) / dt,
        static_cast<double>(s.bytes) / dt, min_us, avg_us, max_us);
}

void setup_realtime() {
    if (kRtCpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(kRtCpu, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
            printf("Warning: failed to pin to CPU %d\n", kRtCpu);
        else
            printf("Pinned to CPU %d\n", kRtCpu);
    }
    struct sched_param param{};
    param.sched_priority = 80;
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
        printf("Warning: failed to set SCHED_FIFO (run as root or grant CAP_SYS_NICE)\n");
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        printf("Warning: mlockall failed\n");
}

// --- Board-specific monitors --------------------------------------------------
class HpmMonitor : public librmcs::agent::RmcsBoardHpm5321 {
public:
    HpmMonitor()
        : librmcs::agent::RmcsBoardHpm5321{{}, {.dangerously_skip_version_checks = true}} {}
private:
    void can0_receive_callback(const librmcs::data::CanDataView& data) override {
        g_stats[kCan0].record(kCan0, data.can_data.size(),
            data.timestamp_us.has_value() ? data.timestamp_us.value() : 0,
            data.timestamp_us.has_value());
    }
    void uart0_receive_callback(const librmcs::data::UartDataView& data) override {
        g_stats[kUart0].record(kUart0, data.uart_data.size());
    }
};

class CBoardMonitor : public librmcs::agent::CBoard {
public:
    CBoardMonitor()
        : librmcs::agent::CBoard{{}, {.dangerously_skip_version_checks = true}} {}
private:
    void can1_receive_callback(const librmcs::data::CanDataView& data) override {
        g_stats[kCan1].record(kCan1, data.can_data.size());
    }
    void can2_receive_callback(const librmcs::data::CanDataView& data) override {
        g_stats[kCan2].record(kCan2, data.can_data.size());
    }
    void uart1_receive_callback(const librmcs::data::UartDataView& data) override {
        g_stats[kUart1].record(kUart1, data.uart_data.size());
    }
    void uart2_receive_callback(const librmcs::data::UartDataView& data) override {
        g_stats[kUart2].record(kUart2, data.uart_data.size());
    }
    void dbus_receive_callback(const librmcs::data::UartDataView& data) override {
        g_stats[kDbus].record(kDbus, data.uart_data.size());
    }
};

} // namespace

int main() {
    std::signal(SIGINT, on_sigint);
    setup_realtime();

    g_start = std::chrono::steady_clock::now();

    printf("RX monitor - auto-detecting board...\n");

    std::unique_ptr<HpmMonitor> hpm;
    std::unique_ptr<CBoardMonitor> cboard;

    try {
        hpm = std::make_unique<HpmMonitor>();
        printf("Connected: RmcsBoardHpm5321 (CAN0, UART0)\n");
    } catch (const std::runtime_error&) {
        try {
            cboard = std::make_unique<CBoardMonitor>();
            printf("Connected: CBoard (CAN1, CAN2, UART1, UART2, DBUS)\n");
        } catch (const std::runtime_error& e) {
            fprintf(stderr, "No compatible device found: %s\n", e.what());
            return 1;
        }
    }

    printf("Listening. Ctrl-C to stop.\n\n");
    printf("%-13s  %-5s  %5s  %9s  %12s\n", "time(ms)", "ch", "bytes", "delta(us)", "hw_ts(us)");
    printf("%-13s  %-5s  %5s  %9s  %12s\n", "--------", "--", "-----", "---------", "----------");

    std::thread print_thread([] {
        LogEntry e;
        while (g_running.load(std::memory_order_relaxed)) {
            while (g_log_queue.pop(e)) {
                const double t_ms = static_cast<double>(e.timestamp_ns) / 1e6;
                const double delta_us = static_cast<double>(e.delta_ns) / 1e3;
                char hw_buf[16];
                const char* hw_ts_str = "          -";
                if (e.has_hw_ts) {
                    snprintf(hw_buf, sizeof(hw_buf), "%12u", e.hw_timestamp_us);
                    hw_ts_str = hw_buf;
                }
                if (e.delta_ns == 0)
                    printf("%13.3f  %-5s  %5u  %9s  %12s\n",
                        t_ms, kChannelName[e.channel], e.size, "  (first)", hw_ts_str);
                else
                    printf("%13.3f  %-5s  %5u  %9.1f  %12s\n",
                        t_ms, kChannelName[e.channel], e.size, delta_us, hw_ts_str);
            }
            std::this_thread::sleep_for(std::chrono::microseconds{500});
        }
        while (g_log_queue.pop(e)) {
            const double t_ms = static_cast<double>(e.timestamp_ns) / 1e6;
            const double delta_us = static_cast<double>(e.delta_ns) / 1e3;
            char hw_buf[16];
            const char* hw_ts_str = "          -";
            if (e.has_hw_ts) {
                snprintf(hw_buf, sizeof(hw_buf), "%12u", e.hw_timestamp_us);
                hw_ts_str = hw_buf;
            }
            if (e.delta_ns == 0)
                printf("%13.3f  %-5s  %5u  %9s  %12s\n",
                    t_ms, kChannelName[e.channel], e.size, "  (first)", hw_ts_str);
            else
                printf("%13.3f  %-5s  %5u  %9.1f  %12s\n",
                    t_ms, kChannelName[e.channel], e.size, delta_us, hw_ts_str);
        }
    });

    auto last_report = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::seconds{5})
            continue;
        const double dt = std::chrono::duration<double>(now - last_report).count();
        last_report = now;
        printf("\n--- 5s summary ---\n");
        for (int i = 0; i < kChannelCount; ++i)
            print_summary(static_cast<Channel>(i), g_stats[i].take(), dt);
        printf("------------------\n\n");
    }

    print_thread.join();
    printf("\nStopped.\n");
    return 0;
}
