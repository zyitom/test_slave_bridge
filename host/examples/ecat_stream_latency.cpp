// P1 validation tool for the rmcs_board EtherCAT stream bridge
// (firmware/rmcs_board/ecat): the core1 firmware echoes the byte stream
// losslessly, so this tool verifies stream integrity end to end (ESC, SSC,
// ARQ, cross-core rings, dual-core boot) and measures round-trip latency and
// throughput of the stream-over-PDO link.
//
// Build:  cmake -DLIBRMCS_ENABLE_SOEM=ON ... (see host/CMakeLists.txt)
// Run:    sudo ./ecat_stream_latency <interface> [seconds]
//
// Every frame carries a monotonic sequence number and the send timestamp; the
// receive path checks bytes exactly and reports an RTT percentile summary.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <span>
#include <thread>
#include <vector>

#include "host/src/transport/transport.hpp"

namespace {

constexpr std::size_t kFrameSize = 64; // representative protocol frame size

struct Stats {
    std::vector<double> rtts_us;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t corrupt = 0;
};

double percentile(std::vector<double>& v, double p) {
    if (v.empty())
        return 0.0;
    const auto i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
    return v[i];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: {} <interface> [seconds]", argv[0]);
        return 1;
    }
    const int duration_s = argc > 2 ? std::atoi(argv[2]) : 10;

    using namespace librmcs::host;
    const transport::soem::ConnectionOptions options{};
    auto link = transport::soem::create_transport(argv[1], options);

    Stats stats;
    std::atomic<std::uint64_t> acked{0};
    std::uint64_t expected_seq = 0;
    std::byte reassembly[kFrameSize];
    std::size_t reassembled = 0;

    link->receive([&](std::span<const std::byte> data) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        // The stream has no framing of its own here; frames are fixed-size.
        for (const std::byte b : data) {
            reassembly[reassembled++] = b;
            if (reassembled < kFrameSize)
                continue;
            reassembled = 0;

            std::uint64_t seq = 0;
            std::int64_t t_send = 0;
            std::memcpy(&seq, reassembly, sizeof(seq));
            std::memcpy(&t_send, reassembly + 8, sizeof(t_send));
            bool ok = seq == expected_seq;
            for (std::size_t i = 16; ok && i < kFrameSize; i++)
                ok = reassembly[i] == static_cast<std::byte>(seq + i);
            if (!ok) {
                stats.corrupt++;
                std::println(stderr, "corrupt/out-of-order frame at seq {}", expected_seq);
            } else {
                stats.rtts_us.push_back(static_cast<double>(now - t_send) / 1e3);
            }
            expected_seq = seq + 1;
            stats.frames++;
            stats.bytes += kFrameSize;
            acked.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::println("streaming {}-byte frames for {} s...", kFrameSize, duration_s);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
    std::uint64_t seq = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        // Keep a small number of frames in flight so latency stays meaningful
        // (unbounded pipelining would measure queueing, not the link).
        if (seq - acked.load(std::memory_order_relaxed) >= 4) {
            std::this_thread::yield();
            continue;
        }
        auto buffer = link->acquire_transmit_buffer();
        if (!buffer)
            continue;
        const auto span = buffer->data();
        const std::int64_t t_send = std::chrono::steady_clock::now().time_since_epoch().count();
        std::memcpy(span.data(), &seq, sizeof(seq));
        std::memcpy(span.data() + 8, &t_send, sizeof(t_send));
        for (std::size_t i = 16; i < kFrameSize; i++)
            span[i] = static_cast<std::byte>(seq + i);
        link->transmit(std::move(buffer), kFrameSize);
        seq++;
    }

    // Give the last frames a moment to come back, then tear down.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    link.reset();

    std::println(
        "frames {}  corrupt {}  throughput {:.1f} KiB/s per direction", stats.frames,
        stats.corrupt, static_cast<double>(stats.bytes) / 1024.0 / duration_s);
    if (!stats.rtts_us.empty()) {
        std::println(
            "rtt us: p50 {:.1f}  p90 {:.1f}  p99 {:.1f}  max {:.1f}  (n={})",
            percentile(stats.rtts_us, 0.50), percentile(stats.rtts_us, 0.90),
            percentile(stats.rtts_us, 0.99),
            *std::max_element(stats.rtts_us.begin(), stats.rtts_us.end()),
            stats.rtts_us.size());
    }
    return stats.corrupt == 0 ? 0 : 2;
}
