// CAN-FD loopback stress test for the rmcs_board EtherCAT bridge (hpm6e8y),
// which now exposes all four physical CAN ports CAN1..CAN4.
//
// WIRING: two independent loopback pairs, each a single CAN bus with a 120 ohm
// terminator at EACH end:
//   pair 0/1 : CAN1_H<->CAN2_H, CAN1_L<->CAN2_L
//   pair 2/3 : CAN3_H<->CAN4_H, CAN3_L<->CAN4_L
// The host floods CAN-FD frames out CAN1 and CAN3; the partner (CAN2 / CAN4)
// receives and ACKs; the firmware forwards every received frame back to the
// host, which checks it. So each pair is stressed one-way (even->odd) with a
// clean, verifiable sequence.
//
// Note on payload width: the librmcs wire protocol carries at most 8 CAN data
// bytes even for FD frames, so these frames are FD-format (BRS: 1 Mbps
// arbitration / 5 Mbps data phase) with an 8-byte payload -- enough to exercise
// the FD data-phase path end to end.
//
// Each frame's payload is 8 bytes: a 32-bit little-endian sequence number plus a
// 32-bit hash of it. The receiver validates the hash (corruption) and the
// sequence continuity (loss / reordering), all in host time.
//
// LATENCY: each frame's send instant is recorded against its sequence number, so
// a returning frame yields a round-trip time measured entirely on the host
// clock -- the host and board clocks are not synchronized, which is exactly why
// the frame has to come back rather than be timestamped on arrival at the board.
// The reported RTT covers the whole chain:
//
//   T0 --EtherCAT--> core1 --CAN1 TX--> wire --CAN2 RX--> core1 --EtherCAT--> T1
//
// i.e. EtherCAT down + firmware + CAN wire time + firmware + EtherCAT up. To
// isolate the CAN segment, run ecat_stream_latency against the LOOPBACK image
// (core1 echoes the byte stream without touching CAN) and subtract its RTT:
// the difference is CAN wire time plus the firmware forwarding path.
//
// Two caveats when reading the numbers:
//  - RATE. This is a stress tool first: at a high rate frames queue behind the
//    stop-and-wait ARQ and their RTT includes that queueing, which measures
//    latency-under-load. For latency alone, use a low rate (e.g. 200) so each
//    frame travels an empty pipe.
//  - QUANTIZATION. The ARQ turns the RTT into a small multiple of the EtherCAT
//    cycle period (tens of us), so percentiles cluster on cycle boundaries
//    rather than forming a smooth distribution. That granularity is the floor
//    of this method, not noise.
//
// Build:  cmake -DLIBRMCS_ENABLE_IGH=ON ... (see host/CMakeLists.txt)
// Run:    sudo ./ecat_canfd_stress <interface> [seconds] [rate_fps_per_stream]
//         The EtherCAT backend defaults to IgH when compiled in; override with
//         RMCS_ECAT_BACKEND=soem|igh. (for steadier pacing:
//         sudo chrt -f 80 ./ecat_canfd_stress enp2s0 30 8000; for latency:
//         sudo chrt -f 80 ./ecat_canfd_stress enp2s0 10 200)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <librmcs/board/rmcs_board_ecat_bridge.hpp>


// CAN ports are named as the ENCLOSURE labels them (1-based), not as the
// 0-based DataId underneath. See librmcs/board/rmcs_can_port.hpp.
using librmcs::board::rmcs::CanPort;

namespace {

// Distinct standard IDs per source so a receiver can attribute frames and a
// miswire (frame arriving on the wrong bus) is visible.
constexpr uint32_t kCanId0 = 0x200; // sent on CAN1, expected on CAN2
constexpr uint32_t kCanId2 = 0x202; // sent on CAN3, expected on CAN4
constexpr size_t kPayloadBytes = 8;
constexpr size_t kCorruptionEvidenceCount = 8;
constexpr auto kDrainTimeout = std::chrono::seconds{2};

// The first RTT samples include ARQ start-up, cold caches and NIC ramp-up; keep
// them out of the percentiles (same rationale as ecat_stream_latency).
constexpr auto kWarmup = std::chrono::milliseconds{500};

// Send instants indexed by sequence number, so a returning frame can be matched
// to its transmission. Both pairs' frames carry the same seq and leave in the
// same packet, so one ring serves both streams. Sized far above any plausible
// in-flight count (rate x RTT), which keeps a slot from being recycled while
// its frame is still on the wire.
constexpr size_t kSendTimeRingSize = 1U << 16;
constexpr uint32_t kSendTimeRingMask = kSendTimeRingSize - 1U;
std::array<std::atomic<int64_t>, kSendTimeRingSize> g_send_time_ns{};

// Set once before the first frame is sent, then read by the poll thread.
std::atomic<int64_t> g_warmup_end_ns{0};

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

// Index into an ALREADY SORTED sample vector. (Sorting once is unambiguous;
// running nth_element per percentile combines with unspecified argument
// evaluation order to produce impossible output like max < p99 -- the same
// trap ecat_stream_latency documents.)
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty())
        return 0.0;
    const auto i = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[i];
}

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

enum class CorruptionKind : uint8_t {
    kLength,
    kHeader,
    kHash,
};

struct CorruptionEvidence {
    std::atomic<bool> ready{false};
    CorruptionKind kind = CorruptionKind::kLength;
    size_t length = 0;
    uint32_t can_id = 0;
    bool is_fdcan = false;
    bool is_extended_can_id = false;
    bool is_remote_transmission = false;
    std::array<std::byte, kPayloadBytes> payload{};
};

// Per loopback pair. Counters are read live by the main thread; the sequence
// state is touched only from the single EtherCAT poll thread that drives the
// receive callbacks. Evidence is published only after its contents are ready.
struct Stream {
    std::atomic<uint64_t> tx{0};
    std::atomic<uint64_t> rx{0};
    std::atomic<uint64_t> lost{0};    // forward gaps in the sequence
    std::atomic<uint64_t> reorder{0}; // a seq at or below one already seen
    std::atomic<uint64_t> corrupt{0}; // bad length / not FD / hash mismatch
    std::atomic<uint64_t> bad_length{0};
    std::atomic<uint64_t> bad_header{0};
    std::atomic<uint64_t> bad_hash{0};
    std::atomic<size_t> evidence_total{0};
    std::array<CorruptionEvidence, kCorruptionEvidenceCount> evidence{};

    bool started = false;
    uint32_t next_expected = 0;

    // Round-trip samples. Like the sequence state above, this is touched only by
    // the poll thread; the main thread reads it after destroying the bridge,
    // which joins that thread (see main()).
    std::vector<double> rtts_us;

    // Match a validated frame to its send instant. Only hash-valid frames get
    // here, so the sequence number can be trusted to index the ring.
    void record_rtt(uint32_t seq, int64_t now_ns) {
        const int64_t sent_ns =
            g_send_time_ns[seq & kSendTimeRingMask].load(std::memory_order_acquire);
        // sent_ns == 0 means the slot was never written, and now_ns < sent_ns
        // means it was overwritten by a sequence exactly one ring apart -- i.e.
        // a frame so late that its slot has been recycled. Neither can happen
        // at any sane rate; discard rather than report a bogus RTT.
        if (sent_ns == 0 || now_ns < sent_ns)
            return;
        if (now_ns < g_warmup_end_ns.load(std::memory_order_relaxed))
            return;
        rtts_us.push_back(static_cast<double>(now_ns - sent_ns) / 1e3);
    }

    void record_corruption(CorruptionKind kind, const librmcs::data::CanDataView& data) {
        corrupt.fetch_add(1, std::memory_order_relaxed);
        const size_t index = evidence_total.fetch_add(1, std::memory_order_relaxed);
        if (index >= evidence.size())
            return;

        CorruptionEvidence& item = evidence[index];
        item.kind = kind;
        item.length = data.can_data.size();
        item.can_id = data.can_id;
        item.is_fdcan = data.is_fdcan;
        item.is_extended_can_id = data.is_extended_can_id;
        item.is_remote_transmission = data.is_remote_transmission;
        const size_t copied = std::min(item.payload.size(), data.can_data.size());
        std::copy_n(data.can_data.begin(), copied, item.payload.begin());
        item.ready.store(true, std::memory_order_release);
    }

    void verify(const librmcs::data::CanDataView& data, uint32_t expected_can_id) {
        // Stamp on entry so frame validation below is not billed to the RTT.
        const int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();

        if (data.can_data.size() != kPayloadBytes) {
            bad_length.fetch_add(1, std::memory_order_relaxed);
            record_corruption(CorruptionKind::kLength, data);
            rx.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!data.is_fdcan || data.is_extended_can_id || data.is_remote_transmission
            || data.can_id != expected_can_id) {
            bad_header.fetch_add(1, std::memory_order_relaxed);
            record_corruption(CorruptionKind::kHeader, data);
            rx.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::byte* p = data.can_data.data();
        const uint32_t seq = get_u32_le(p);
        if (get_u32_le(p + 4) != mix(seq)) {
            bad_hash.fetch_add(1, std::memory_order_relaxed);
            record_corruption(CorruptionKind::kHash, data);
            rx.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Every intact frame carries a usable RTT, whether or not it arrives in
        // order, so this precedes the sequence bookkeeping below.
        record_rtt(seq, now_ns);

        if (!started) {
            started = true;
            lost.fetch_add(seq, std::memory_order_relaxed);
            next_expected = seq + 1;
            rx.fetch_add(1, std::memory_order_relaxed);
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
        rx.fetch_add(1, std::memory_order_relaxed);
    }
};

class Receiver final : public librmcs::board::RmcsBoardEcatBridge::Callback {
public:
    Stream pair01; // CAN1 -> CAN2
    Stream pair23; // CAN3 -> CAN4
    std::atomic<uint64_t> unexpected{0};

private:
    void can_receive(
        CanPort port, const librmcs::data::CanDataView& data) override {
        switch (port) {
        case CanPort::kCan2: pair01.verify(data, kCanId0); break;
        case CanPort::kCan4: pair23.verify(data, kCanId2); break;
        // The transmitters never receive their own frames on a one-way bus;
        // anything landing here signals a miswire or an unexpected bus partner.
        case CanPort::kCan1: {
        case CanPort::kCan3:
            unexpected.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        default: break;
        }
    }
};

void print_line(const char* tag, const Stream& s, uint64_t rx_delta, double dt_s) {
    const double fps = dt_s > 0 ? static_cast<double>(rx_delta) / dt_s : 0.0;
    const uint64_t tx = s.tx.load();
    const uint64_t received = s.rx.load();
    const uint64_t missing = tx > received ? tx - received : 0;
    printf(
        "  %s  tx=%-10llu rx=%-10llu missing=%-8llu gap=%-8llu corrupt=%-6llu"
        " reorder=%-6llu  %8.0f f/s  %6.2f KiB/s\n",
        tag, static_cast<unsigned long long>(tx), static_cast<unsigned long long>(received),
        static_cast<unsigned long long>(missing),
        static_cast<unsigned long long>(s.lost.load()),
        static_cast<unsigned long long>(s.corrupt.load()),
        static_cast<unsigned long long>(s.reorder.load()), fps,
        fps * kPayloadBytes / 1024.0);
}

// Sorts the samples in place, so call it once, after the poll thread is joined.
void print_latency(const char* tag, Stream& stream) {
    if (stream.rtts_us.empty()) {
        printf("  %s  rtt: no samples (nothing returned after the %lld ms warm-up)\n", tag,
               static_cast<long long>(kWarmup.count()));
        return;
    }
    std::sort(stream.rtts_us.begin(), stream.rtts_us.end());
    printf(
        "  %s  rtt us: p50 %-8.1f p90 %-8.1f p99 %-8.1f p99.9 %-8.1f max %-8.1f (n=%zu)\n", tag,
        percentile(stream.rtts_us, 0.50), percentile(stream.rtts_us, 0.90),
        percentile(stream.rtts_us, 0.99), percentile(stream.rtts_us, 0.999),
        stream.rtts_us.back(), stream.rtts_us.size());
}

const char* corruption_kind_name(CorruptionKind kind) {
    switch (kind) {
    case CorruptionKind::kLength: return "length";
    case CorruptionKind::kHeader: return "header";
    case CorruptionKind::kHash: return "hash";
    }
    return "unknown";
}

void print_corruption_evidence(const char* tag, const Stream& stream) {
    if (stream.corrupt.load(std::memory_order_relaxed) == 0)
        return;

    printf(
        "  %s corruption classes: length=%llu header=%llu hash=%llu\n", tag,
        static_cast<unsigned long long>(stream.bad_length.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(stream.bad_header.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(stream.bad_hash.load(std::memory_order_relaxed)));

    const size_t count = std::min(
        stream.evidence_total.load(std::memory_order_relaxed), stream.evidence.size());
    for (size_t index = 0; index < count; ++index) {
        const CorruptionEvidence& item = stream.evidence[index];
        if (!item.ready.load(std::memory_order_acquire))
            continue;

        printf(
            "    bad[%zu] kind=%s len=%zu id=0x%08x fd=%u ext=%u rtr=%u payload=", index,
            corruption_kind_name(item.kind), item.length, item.can_id,
            static_cast<unsigned>(item.is_fdcan),
            static_cast<unsigned>(item.is_extended_can_id),
            static_cast<unsigned>(item.is_remote_transmission));
        const size_t payload_size = std::min(item.length, item.payload.size());
        for (size_t byte_index = 0; byte_index < payload_size; ++byte_index)
            printf("%02x", std::to_integer<unsigned>(item.payload[byte_index]));
        if (item.length == kPayloadBytes) {
            const uint32_t seq = get_u32_le(item.payload.data());
            printf(
                " seq=%u hash=0x%08x expected=0x%08x", seq,
                get_u32_le(item.payload.data() + 4), mix(seq));
        }
        putchar('\n');
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <interface> [seconds] [rate_fps_per_stream]\n", argv[0]);
        return 1;
    }
    const int duration_s = argc > 2 ? std::atoi(argv[2]) : 10;
    const uint32_t rate = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 5000;
    if (duration_s <= 0 || rate == 0U || rate > 1'000'000U) {
        fprintf(stderr, "seconds must be positive and rate must be in [1, 1000000]\n");
        return 1;
    }

    std::signal(SIGINT, on_sigint);

    Receiver rx;
    try {
        // Session establishment happens inside the constructor; returning means
        // EtherCAT is OPERATIONAL and the protocol handshake passed. Held in an
        // optional (the bridge is immovable, so emplace constructs in place) to
        // make the teardown explicit: destroying it joins the poll thread, which
        // is what makes the latency samples safe to read afterwards.
        std::optional<librmcs::board::RmcsBoardEcatBridge> board;
        board.emplace(argv[1], rx);
        printf("EtherCAT bridge connected on %s, session established.\n", argv[1]);
        printf(
            "CAN-FD stress: CAN1->CAN2 and CAN3->CAN4, target %u f/s per stream for %d s "
            "(Ctrl-C to stop early)\n\n",
            rate, duration_s);

        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        // Safe to publish before any frame is sent: nothing can return yet.
        g_warmup_end_ns.store(
            (start + kWarmup).time_since_epoch().count(), std::memory_order_relaxed);
        const auto deadline = start + std::chrono::seconds{duration_s};
        const auto send_period = std::chrono::duration_cast<clock::duration>(
            std::chrono::nanoseconds{1'000'000'000U / rate});
        auto next_send = start;
        auto next_report = start + std::chrono::seconds{1};
        uint64_t last_rx01 = 0, last_rx23 = 0;
        auto last_report = start;

        uint64_t sent = 0;
        uint64_t missed_pacing_slots = 0;
        while (clock::now() < deadline && !g_stop.load(std::memory_order_relaxed)) {
            auto now = clock::now();
            const double elapsed_s = std::chrono::duration<double>(now - start).count();

            if (now >= next_send) {
                const auto seq = static_cast<uint32_t>(sent);
                std::byte a[kPayloadBytes];
                std::byte b[kPayloadBytes];
                encode(a, seq);
                encode(b, seq);

                // Publish the send instant before handing the frames to the
                // transport, so the slot is always populated by the time the
                // poll thread can see the frame come back.
                g_send_time_ns[seq & kSendTimeRingMask].store(
                    clock::now().time_since_epoch().count(), std::memory_order_release);

                // One packet carries both pairs' frames. The builder API does
                // not surface transport enqueue failure; the final tx/rx
                // equality check catches a command lost at any layer.
                board->start_transmit()
                    .can_transmit(CanPort::kCan1, 
                        {.can_id = kCanId0, .can_data = a, .is_fdcan = true})
                    .can_transmit(CanPort::kCan3, 
                        {.can_id = kCanId2, .can_data = b, .is_fdcan = true});

                rx.pair01.tx.fetch_add(1, std::memory_order_relaxed);
                rx.pair23.tx.fetch_add(1, std::memory_order_relaxed);
                ++sent;

                next_send += send_period;
                now = clock::now();
                if (next_send <= now) {
                    const auto skipped =
                        static_cast<uint64_t>((now - next_send) / send_period) + 1U;
                    missed_pacing_slots += skipped;
                    next_send += send_period * skipped;
                }
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

            auto wake_time = std::min(next_send, next_report);
            wake_time = std::min(wake_time, deadline);
            if (wake_time > clock::now())
                std::this_thread::sleep_until(wake_time);
        }

        // Drain queued transport and CAN traffic, but do not hang forever when
        // the tail was genuinely dropped.
        const auto drain_deadline = clock::now() + kDrainTimeout;
        while (clock::now() < drain_deadline
               && (rx.pair01.rx.load() < rx.pair01.tx.load()
                   || rx.pair23.rx.load() < rx.pair23.tx.load())) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        // Stop the transport before touching the latency samples: rtts_us is
        // owned by the poll thread, and this join is what ends the race.
        // Counters stay readable -- they are atomics.
        board.reset();

        printf("\n=== summary ===\n");
        print_line("01", rx.pair01, 0, 0);
        print_line("23", rx.pair23, 0, 0);
        print_latency("01", rx.pair01);
        print_latency("23", rx.pair23);
        print_corruption_evidence("01", rx.pair01);
        print_corruption_evidence("23", rx.pair23);
        printf("  missed pacing slots: %llu\n",
               static_cast<unsigned long long>(missed_pacing_slots));
        const uint64_t unexpected = rx.unexpected.load();
        if (unexpected != 0)
            printf("  WARNING: %llu frame(s) arrived on a transmitter's own bus (miswire?)\n",
                   static_cast<unsigned long long>(unexpected));

        const bool clean = rx.pair01.tx.load() > 0 && rx.pair23.tx.load() > 0
                        && rx.pair01.rx.load() == rx.pair01.tx.load()
                        && rx.pair23.rx.load() == rx.pair23.tx.load()
                        && rx.pair01.corrupt.load() == 0 && rx.pair23.corrupt.load() == 0
                        && rx.pair01.lost.load() == 0 && rx.pair23.lost.load() == 0
                        && rx.pair01.reorder.load() == 0 && rx.pair23.reorder.load() == 0
                        && unexpected == 0;
        printf("  result: %s\n", clean ? "PASS (no loss, no corruption)"
                                        : "CHECK counters above");
        return clean ? 0 : 2;
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
