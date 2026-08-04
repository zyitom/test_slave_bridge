// USB HS packet-rate probe -- how many bulk OUT packets per second the host can
// actually push to one board, as opposed to how many CAN frames the wire can
// carry.
//
// WHY THIS EXISTS: `dual_board_test stress` batches CAN0 and CAN1 into ONE USB
// packet per iteration, so a run at 19000 frames/s per bus only ever produced
// 19000 packets/s. That measured the CAN wire (which saturates at ~19870
// frames/s for an 8-byte CAN-FD frame at 1M/5M) and said nothing about the USB
// transaction ceiling. This tool sends each record in its OWN packet, so the
// packet rate can be raised without touching the per-bus frame rate.
//
// WHY THE NUMBER MEANS SOMETHING: the USB transport's acquire_transmit_buffer()
// blocks on a condition variable until one of the 64 pooled transfers is
// returned by its completion callback. Submission is therefore back-pressured by
// real USB completions -- a flood loop measures what the controller retires, not
// how fast memcpy runs.
//
// MODES
//   combined  can0+can1 in one packet         1 packet  / 2 CAN frames
//   split     can0 and can1 in own packets    2 packets / 2 CAN frames
//   split3    can0, can1, uart0 each its own  3 packets / 2 CAN frames + 1 UART
//
// Pass rate 0 to flood (no pacing): the loop then reports the ceiling the pool
// back-pressure allows. Any positive rate paces to that many ITERATIONS per
// second, so packets/s is rate x (records per iteration).
//
// Frames dropped by the board's MCAN TX FIFO do not invalidate a flood run: the
// USB packet still crossed the wire, which is the thing being measured. The
// per-bus received counts are printed so CAN-side saturation stays visible and
// is never mistaken for a USB limit.
//
// Needs root (raw USB device access).
//   sudo ./usb_packet_rate <mode> [iterations_per_sec] [seconds]

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>
#include <pthread.h>
#include <sched.h>

#include <librmcs/board/common.hpp>
#include <librmcs/board/rmcs_board_hpm5321_dual_can.hpp>

namespace {

using Board = librmcs::board::RmcsBoardHpm5321DualCan;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kCanIdBase = 0x560;
constexpr size_t kPayloadSize = 8;
constexpr size_t kUartPayloadSize = 8;

std::string g_serial_a;
std::string g_serial_b;

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

struct NodeOptions final : librmcs::board::AdvancedOptions {
    int io_core = -1;
    const char* thread_name = nullptr;
};

// Counts what actually landed, per bus, so CAN-side saturation is never
// mistaken for a USB ceiling.
struct Counter {
    std::atomic<uint64_t> can[2] = {};
    std::atomic<uint64_t> uart_bytes = 0;
};

class Node final : public Board::Callback {
public:
    Node(std::string_view serial, int io_core, const char* thread_name) {
        options_.io_core = io_core;
        options_.thread_name = thread_name;
        options_.dangerously_skip_version_checks = true;
        options_.thread_setup = [](const librmcs::board::AdvancedOptions& self) noexcept {
            const auto& options = static_cast<const NodeOptions&>(self);
            configure_thread(options.io_core, 90, options.thread_name);
        };
        board_ = std::make_unique<Board>(*this, serial, options_);
    }

    Board& board() { return *board_; }
    void watch(Counter* counter) { counter_.store(counter, std::memory_order_release); }

private:
    void can0_receive_callback(const librmcs::data::CanDataView& data) override {
        tally_can(0, data);
    }
    void can1_receive_callback(const librmcs::data::CanDataView& data) override {
        tally_can(1, data);
    }
    void uart0_receive_callback(const librmcs::data::UartDataView& data) override {
        if (Counter* counter = counter_.load(std::memory_order_acquire))
            counter->uart_bytes.fetch_add(data.uart_data.size(), std::memory_order_relaxed);
    }

    void tally_can(int bus, const librmcs::data::CanDataView& data) {
        if (data.can_data.size() != kPayloadSize)
            return;
        if (Counter* counter = counter_.load(std::memory_order_acquire))
            counter->can[bus].fetch_add(1, std::memory_order_relaxed);
    }

    NodeOptions options_;
    std::unique_ptr<Board> board_;
    std::atomic<Counter*> counter_ = nullptr;
};

// Sysfs scan rather than a libusb enumeration, so discovery costs nothing and
// matches dual_board_test's A/B assignment exactly (sorted serial order).
std::vector<std::string> enumerate_serials() {
    std::vector<std::string> found;
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir)
        return found;
    while (const dirent* entry = readdir(dir)) {
        const std::string base = std::string{"/sys/bus/usb/devices/"} + entry->d_name;
        const auto read_line = [&](const char* leaf) -> std::string {
            const std::string path = base + "/" + leaf;
            FILE* file = fopen(path.c_str(), "re");
            if (!file)
                return {};
            char buffer[256] = {};
            if (!fgets(buffer, sizeof(buffer), file)) {
                fclose(file);
                return {};
            }
            fclose(file);
            std::string value{buffer};
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
                value.pop_back();
            return value;
        };
        if (read_line("idVendor") != "a11c" || read_line("idProduct") != "a902")
            continue;
        std::string serial = read_line("serial");
        if (!serial.empty())
            found.push_back(std::move(serial));
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    return found;
}

bool discover() {
    const auto serials = enumerate_serials();
    if (serials.size() < 2) {
        fprintf(
            stderr, "need two hpm5321_dual_can boards (a11c:a902), found %zu\n", serials.size());
        return false;
    }
    g_serial_a = serials[0];
    g_serial_b = serials[1];
    return true;
}

enum class Mode { kCombined, kSplit, kSplit3 };

int records_per_iteration(Mode mode) {
    switch (mode) {
    case Mode::kCombined: return 1; // one packet carrying two CAN records
    case Mode::kSplit: return 2;
    case Mode::kSplit3: return 3;
    }
    return 1;
}

// One sender's loop. Returns the packets it pushed. `deadline` is shared so two
// senders measure the same window.
uint64_t send_loop(Node& node, Mode mode, uint32_t iterations_per_sec, Clock::time_point deadline) {
    std::byte can_payload[kPayloadSize] = {};
    std::byte uart_payload[kUartPayloadSize] = {};

    const bool flood = iterations_per_sec == 0;
    const auto period = flood ? std::chrono::nanoseconds{0}
                              : std::chrono::nanoseconds{1'000'000'000ULL / iterations_per_sec};

    uint64_t packets = 0;
    auto next = Clock::now();

    while (Clock::now() < deadline) {
        // Each `start_transmit()` scope flushes exactly one USB packet when the
        // builder goes out of scope -- that is what makes split modes cost more
        // packets for the same number of CAN frames.
        if (mode == Mode::kCombined) {
            auto builder = node.board().start_transmit();
            builder.can0_transmit(
                {.can_id = kCanIdBase, .can_data = can_payload, .is_fdcan = true});
            builder.can1_transmit(
                {.can_id = kCanIdBase + 1, .can_data = can_payload, .is_fdcan = true});
            packets += 1;
        } else {
            {
                auto builder = node.board().start_transmit();
                builder.can0_transmit(
                    {.can_id = kCanIdBase, .can_data = can_payload, .is_fdcan = true});
            }
            {
                auto builder = node.board().start_transmit();
                builder.can1_transmit(
                    {.can_id = kCanIdBase + 1, .can_data = can_payload, .is_fdcan = true});
            }
            packets += 2;
            if (mode == Mode::kSplit3) {
                auto builder = node.board().start_transmit();
                builder.uart0_transmit({.uart_data = uart_payload, .idle_delimited = false});
                packets += 1;
            }
        }

        if (!flood) {
            next += period;
            while (Clock::now() < next) {}
        }
    }
    return packets;
}

int run(Mode mode, uint32_t iterations_per_sec, uint32_t seconds, bool both) {
    Node board_a{g_serial_a, 7, "rate-a"};
    Node board_b{g_serial_b, 6, "rate-b"};
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    Counter counter;
    board_b.watch(&counter);

    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds{seconds};

    // `both` puts a second sender on the SECOND board. Both boards sit on the
    // same xHCI controller, so this is the test that separates a per-device
    // ceiling from a per-controller one: if the aggregate roughly doubles the
    // ceiling is per device, if it stays flat the controller is the limit.
    uint64_t packets_b = 0;
    std::thread sender_b;
    if (both) {
        sender_b = std::thread{[&] {
            configure_thread(-1, 80, "rate-txb");
            packets_b = send_loop(board_b, mode, iterations_per_sec, deadline);
        }};
    }

    const uint64_t packets_a = send_loop(board_a, mode, iterations_per_sec, deadline);
    if (sender_b.joinable())
        sender_b.join();

    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    const char* name = mode == Mode::kCombined ? "combined"
                     : mode == Mode::kSplit    ? "split"
                                               : "split3";
    printf(
        "%-9s %-6s %-7s packets/s A %8.0f", name, both ? "2-brd" : "1-brd",
        iterations_per_sec == 0 ? "flood" : "paced", static_cast<double>(packets_a) / elapsed);
    if (both)
        printf(
            "  B %8.0f  TOTAL %8.0f", static_cast<double>(packets_b) / elapsed,
            static_cast<double>(packets_a + packets_b) / elapsed);
    printf("  (%d records/iteration)\n", records_per_iteration(mode));
    printf(
        "                          received on B  bus0 %8.0f f/s  bus1 %8.0f f/s  uart %8.0f B/s\n",
        static_cast<double>(counter.can[0].load()) / elapsed,
        static_cast<double>(counter.can[1].load()) / elapsed,
        static_cast<double>(counter.uart_bytes.load()) / elapsed);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(
            stderr, "usage: usb_packet_rate <combined|split|split3> [iterations_per_sec] [seconds] "
                    "[both]\n"
                    "  iterations_per_sec 0 (default) floods with no pacing\n"
                    "  both               drive BOTH boards at once, to tell a per-device ceiling\n"
                    "                     from a per-controller one\n");
        return 1;
    }
    const std::string_view mode_name{argv[1]};
    Mode mode;
    if (mode_name == "combined")
        mode = Mode::kCombined;
    else if (mode_name == "split")
        mode = Mode::kSplit;
    else if (mode_name == "split3")
        mode = Mode::kSplit3;
    else {
        fprintf(
            stderr, "unknown mode: %.*s\n", static_cast<int>(mode_name.size()), mode_name.data());
        return 1;
    }

    const uint32_t rate = argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0)) : 0;
    const uint32_t seconds =
        argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 8;
    const bool both = argc > 4 && std::string_view{argv[4]} == "both";

    try {
        if (!discover())
            return 1;
        configure_thread(-1, 80, "rate-main");
        return run(mode, rate, seconds, both);
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}
