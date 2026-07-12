// Firmware-faithful simulation of the go-back-N stream: slave echoes the
// byte stream (core1 loopback) and performs the doorbell/main-loop EXTRA
// build calls between frames, exactly like rmcs_input_refresh(). The master
// runs the igh.cpp cycle shape (build -> frame -> on_peer_chunk) with an
// inflight=1 64-byte echo workload mirroring ecat_stream_latency.
//
// Build (standalone, not part of the CMake build):
//   g++ -O2 -std=c++23 -Icore/include host/src/transport/igh/reference/pd_stream_sim.cpp -o pd_sim
// Run: ./pd_sim [cycles] [loss_ratio] [doorbell_builds]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <span>
#include <vector>

#include <librmcs/ecat/pd_stream.hpp>

using librmcs::ecat::kPdChunkPayloadSize;
using librmcs::ecat::kPdChunkSize;
using librmcs::ecat::PdStreamEndpoint;

namespace {

constexpr std::size_t kFrameSize = 64;

// Bounded byte FIFO with the Ring interface both endpoint sides expect.
struct ByteRing {
    std::deque<std::byte> q;
    std::size_t capacity = 1024;

    bool try_push(std::span<const std::byte> data) noexcept {
        if (q.size() + data.size() > capacity)
            return false;
        for (const std::byte b : data)
            q.push_back(b);
        return true;
    }
    std::size_t pop(std::span<std::byte> destination) noexcept {
        const std::size_t n = std::min(destination.size(), q.size());
        for (std::size_t i = 0; i < n; i++) {
            destination[i] = q.front();
            q.pop_front();
        }
        return n;
    }
    std::size_t readable() const noexcept { return q.size(); }
};

struct FrameChecker {
    std::uint64_t expected_seq = 0;
    std::uint64_t frames = 0;
    std::uint64_t corrupt = 0;
    std::vector<std::byte> reassembly;

    bool try_push(std::span<const std::byte> data) noexcept {
        for (const std::byte b : data) {
            reassembly.push_back(b);
            if (reassembly.size() == kFrameSize) {
                std::uint64_t seq = 0;
                std::memcpy(&seq, reassembly.data(), sizeof(seq));
                bool ok = seq == expected_seq;
                for (std::size_t i = 16; ok && i < kFrameSize; i++)
                    ok = reassembly[i] == static_cast<std::byte>(seq + i);
                frames++;
                if (!ok)
                    corrupt++;
                expected_seq = seq + 1;
                reassembly.clear();
            }
        }
        return true;
    }
};

void make_frame(std::uint64_t seq, std::byte* out) {
    std::memcpy(out, &seq, sizeof(seq));
    std::memset(out + 8, 0, 8);
    for (std::size_t i = 16; i < kFrameSize; i++)
        out[i] = static_cast<std::byte>(seq + i);
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 500000;
    const double loss = argc > 2 ? std::strtod(argv[2], nullptr) : 0.0;
    const int doorbell_builds = argc > 3 ? std::atoi(argv[3]) : 2;

    PdStreamEndpoint master;
    PdStreamEndpoint slave;
    ByteRing master_tx;  // app -> master endpoint
    ByteRing slave_up;   // core1 echo -> slave endpoint
    ByteRing slave_down; // slave endpoint -> core1 (echo input)
    FrameChecker checker;

    std::byte outputs[kPdChunkSize] = {};
    std::byte inputs[kPdChunkSize] = {};
    std::byte frame_buffer[kFrameSize];
    std::byte shuttle[256];

    std::mt19937_64 rng{7};
    std::uniform_real_distribution<double> uni{0.0, 1.0};

    // inflight = 1: one frame in the pipe, next sent when the echo returns.
    std::uint64_t send_seq = 0;
    std::uint64_t received_before = 0;
    make_frame(send_seq++, frame_buffer);
    (void)master_tx.try_push({frame_buffer, kFrameSize});

    for (std::uint64_t n = 0; n < cycles; n++) {
        master.build_own_chunk(outputs, master_tx);
        if (uni(rng) < loss)
            continue; // frame lost on the wire

        // Frame passes the slave: reads the CURRENT input image, delivers
        // the outputs; the PDI ISR then consumes and rebuilds the inputs.
        received_before = checker.frames;
        master.on_peer_chunk(inputs, checker);
        slave.on_peer_chunk(outputs, slave_down);
        slave.build_own_chunk(inputs, slave_up);

        // core1: echo everything that arrived, then ring the doorbell --
        // extra InputMapping passes between frames, gated exactly like
        // rmcs_pd_uplink_pending().
        const std::size_t echoed = slave_down.pop(shuttle);
        if (echoed != 0)
            (void)slave_up.try_push({shuttle, echoed});
        for (int d = 0; d < doorbell_builds; d++)
            if (slave.ready_to_advance() && slave_up.readable() != 0)
                slave.build_own_chunk(inputs, slave_up);

        // App reaction (response window): a completed echo triggers the next
        // frame immediately, riding the next cycle's build.
        if (checker.frames != received_before) {
            make_frame(send_seq++, frame_buffer);
            (void)master_tx.try_push({frame_buffer, kFrameSize});
        }
    }

    std::printf(
        "cycles %llu  loss %.2f%%  doorbell_builds %d\n"
        "frames %llu  corrupt %llu  cycles/frame %.2f\n",
        static_cast<unsigned long long>(cycles), 100.0 * loss, doorbell_builds,
        static_cast<unsigned long long>(checker.frames),
        static_cast<unsigned long long>(checker.corrupt),
        checker.frames ? static_cast<double>(cycles) / static_cast<double>(checker.frames) : 0.0);
    return checker.corrupt == 0 ? 0 : 1;
}
