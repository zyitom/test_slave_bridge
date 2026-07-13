// CAN-FD loopback stress test for the HPM6E8Y board over USB (PID 0xA904) --
// the USB-transport twin of ecat_canfd_stress. Same wiring and same checks, so
// the two can be compared directly to see how the transport (USB vendor bulk
// vs EtherCAT ARQ stream) affects CAN loopback loss/throughput.
//
// WIRING: two independent loopback pairs, each a single CAN bus with a 120 ohm
// terminator at EACH end:
//   pair 0/1 : CAN0_H<->CAN1_H, CAN0_L<->CAN1_L
//   pair 2/3 : CAN2_H<->CAN3_H, CAN2_L<->CAN3_L
// The host floods CAN-FD frames out CAN0 and CAN2; the partner (CAN1 / CAN3)
// receives, and the firmware forwards every received frame back to the host,
// which checks it.
//
// Each frame's payload is 8 bytes: a 32-bit little-endian sequence number plus a
// 32-bit hash of it. The receiver validates the hash (corruption) and the
// sequence continuity (loss / reordering), all in host time.
//
// Build:  cmake --preset linux-debug -S host   (no EtherCAT backend needed)
// Run:    sudo ./usb_canfd_stress [seconds] [rate_fps_per_stream] [serial_filter]
//         (for steadier pacing:  sudo chrt -f 80 ./usb_canfd_stress 30 8000)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <string_view>
#include <thread>

#include <librmcs/board/rmcs_board_hpm6e8y.hpp>

namespace {

// Distinct standard IDs per source so a receiver can attribute frames and a
// miswire (frame arriving on the wrong bus) is visible.
constexpr uint32_t kCanId0 = 0x200; // sent on CAN0, expected on CAN1
constexpr uint32_t kCanId2 = 0x202; // sent on CAN2, expected on CAN3
constexpr size_t kPayloadBytes = 8;

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

// A cheap 32-bit avalanche mix, so a single flipped payload bit is caught.
uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void put_u32_le(std::byte* p, uint32_t v) {
    p[0] = static_cast<std::byte>(v);
    p[1] = static_cast<std::byte>(v >> 8);
    p[2] = static_cast<std::byte>(v >> 16);
    p[3] = static_cast<std::byte>(v >> 24);
}

uint32_t get_u32_le(const std::byte* p) {
    return static_cast<uint32_t>(std::to_integer<uint8_t>(p[0]))
         | static_cast<uint32_t>(std::to_integer<uint8_t>(p[1])) << 8
         | static_cast<uint32_t>(std::to_integer<uint8_t>(p[2])) << 16
         | static_cast<uint32_t>(std::to_integer<uint8_t>(p[3])) << 24;
}

// Encodes seq + hash(seq) into an 8-byte frame payload.
void encode(std::byte* out, uint32_t seq) {
    put_u32_le(out, seq);
    put_u32_le(out + 4, mix(seq));
}

// Per loopback pair. Counters are read live by the main thread; the sequence
// state is touched only from the single USB receive thread, so it needs no
// synchronization.
struct Stream {
    std::atomic<uint64_t> tx{0};
    std::atomic<uint64_t> rx{0};
    std::atomic<uint64_t> lost{0};    // forward gaps in the sequence
    std::atomic<uint64_t> reorder{0}; // a seq at or below one already seen
    std::atomic<uint64_t> corrupt{0}; // bad length / not FD / hash mismatch

    bool started = false;
    uint32_t next_expected = 0;

    void verify(const librmcs::data::CanDataView& data) {
        rx.fetch_add(1, std::memory_order_relaxed);

        if (data.can_data.size() != kPayloadBytes || !data.is_fdcan) {
            corrupt.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::byte* p = data.can_data.data();
        const uint32_t seq = get_u32_le(p);
        if (get_u32_le(p + 4) != mix(seq)) {
            corrupt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (!started) {
            started = true;
            next_expected = seq + 1;
            return;
        }
        if (seq == next_expected) {
            next_expected = seq + 1;
        } else if (seq > next_expected) {
            lost.fetch_add(seq - next_expected, std::memory_order_relaxed);
            next_expected = seq + 1;
        } else {
            reorder.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

class Receiver final : public librmcs::board::RmcsBoardHpm6e8y::Callback {
public:
    Stream pair01; // CAN0 -> CAN1
    Stream pair23; // CAN2 -> CAN3
    std::atomic<uint64_t> unexpected{0};

private:
    void can1_receive_callback(const librmcs::data::CanDataView& data) override {
        pair01.verify(data);
    }
    void can3_receive_callback(const librmcs::data::CanDataView& data) override {
        pair23.verify(data);
    }
    // The transmitters never receive their own frames on a one-way bus; anything
    // landing here signals a miswire or an unexpected bus partner.
    void can0_receive_callback(const librmcs::data::CanDataView&) override {
        unexpected.fetch_add(1, std::memory_order_relaxed);
    }
    void can2_receive_callback(const librmcs::data::CanDataView&) override {
        unexpected.fetch_add(1, std::memory_order_relaxed);
    }
};

void print_line(const char* tag, const Stream& s, uint64_t rx_delta, double dt_s) {
    const double fps = dt_s > 0 ? static_cast<double>(rx_delta) / dt_s : 0.0;
    printf(
        "  %s  tx=%-10llu rx=%-10llu lost=%-8llu corrupt=%-6llu reorder=%-6llu"
        "  %8.0f f/s  %6.2f KiB/s\n",
        tag, static_cast<unsigned long long>(s.tx.load()),
        static_cast<unsigned long long>(s.rx.load()),
        static_cast<unsigned long long>(s.lost.load()),
        static_cast<unsigned long long>(s.corrupt.load()),
        static_cast<unsigned long long>(s.reorder.load()), fps,
        fps * kPayloadBytes / 1024.0);
}

} // namespace

int main(int argc, char** argv) {
    const int duration_s = argc > 1 ? std::atoi(argv[1]) : 10;
    const uint32_t rate = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 5000;
    const std::string_view serial_filter = argc > 3 ? argv[3] : std::string_view{};

    std::signal(SIGINT, on_sigint);

    Receiver rx;
    try {
        // The EtherCAT-bridge firmware advertises its product string as
        // "RMCS EtherCAT Bridge v<ver>" rather than the "RMCS Agent v<ver>" the
        // USB transport's exact-match version check expects. The protocol
        // version itself matches (same build), only the product name differs, so
        // skip the string check here.
        librmcs::board::AdvancedOptions options;
        options.set_dangerously_skip_version_checks(true);

        // Session establishment happens inside the constructor; returning means
        // the USB device is open and the protocol handshake passed.
        librmcs::board::RmcsBoardHpm6e8y board{rx, serial_filter, options};
        printf("HPM6E8Y USB board connected, session established.\n");
        printf(
            "CAN-FD stress: CAN0->CAN1 and CAN2->CAN3, target %u f/s per stream for %d s "
            "(Ctrl-C to stop early)\n\n",
            rate, duration_s);

        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        const auto deadline = start + std::chrono::seconds{duration_s};
        auto next_report = start + std::chrono::seconds{1};
        uint64_t last_rx01 = 0, last_rx23 = 0;
        auto last_report = start;

        uint64_t sent = 0;
        while (clock::now() < deadline && !g_stop.load(std::memory_order_relaxed)) {
            const auto now = clock::now();
            const double elapsed_s = std::chrono::duration<double>(now - start).count();
            const auto target = static_cast<uint64_t>(elapsed_s * rate);

            while (sent < target && clock::now() < deadline
                   && !g_stop.load(std::memory_order_relaxed)) {
                std::byte a[kPayloadBytes];
                std::byte b[kPayloadBytes];
                encode(a, static_cast<uint32_t>(sent));
                encode(b, static_cast<uint32_t>(sent));

                // One packet carries both pairs' frames. write_can never throws
                // on backpressure (a full downlink is dropped and counted as a
                // sequence gap on the receive side), so no try/catch is needed.
                board.start_transmit()
                    .can0_transmit(
                        {.can_id = kCanId0, .can_data = a, .is_fdcan = true})
                    .can2_transmit(
                        {.can_id = kCanId2, .can_data = b, .is_fdcan = true});

                rx.pair01.tx.fetch_add(1, std::memory_order_relaxed);
                rx.pair23.tx.fetch_add(1, std::memory_order_relaxed);
                ++sent;
            }

            if (now >= next_report) {
                const double dt = std::chrono::duration<double>(now - last_report).count();
                const uint64_t rx01 = rx.pair01.rx.load();
                const uint64_t rx23 = rx.pair23.rx.load();
                printf("[%5.1fs]\n", elapsed_s);
                print_line("01", rx.pair01, rx01 - last_rx01, dt);
                print_line("23", rx.pair23, rx23 - last_rx23, dt);
                last_rx01 = rx01;
                last_rx23 = rx23;
                last_report = now;
                next_report += std::chrono::seconds{1};
            }

            std::this_thread::sleep_for(std::chrono::microseconds{200});
        }

        // Let the last in-flight frames arrive before the summary.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        printf("\n=== summary ===\n");
        print_line("01", rx.pair01, 0, 0);
        print_line("23", rx.pair23, 0, 0);
        const uint64_t unexpected = rx.unexpected.load();
        if (unexpected != 0)
            printf("  WARNING: %llu frame(s) arrived on a transmitter's own bus (miswire?)\n",
                   static_cast<unsigned long long>(unexpected));

        const bool clean = rx.pair01.corrupt.load() == 0 && rx.pair23.corrupt.load() == 0
                        && rx.pair01.lost.load() == 0 && rx.pair23.lost.load() == 0
                        && unexpected == 0 && rx.pair01.rx.load() > 0 && rx.pair23.rx.load() > 0;
        printf("  result: %s\n", clean ? "PASS (no loss, no corruption)"
                                        : "CHECK counters above");
        return clean ? 0 : 2;
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
