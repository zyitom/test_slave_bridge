// Queue-free CAN-FD loopback latency over the NATIVE CAN mailbox firmware
// (RMCS_ECAT_NATIVE_CAN): the 48-byte PDO is four 12-byte per-bus mailboxes,
// latest-wins, no ARQ. This is the native twin of examples/bridge_can_loopback
// _latency.cpp and is measured the same way (free-running cycle thread + a
// busy-waiting sender, one frame in flight) so the RTT is directly comparable
// to that tool's `ecat`/`usb` numbers.
//
// Wire CAN0 and CAN1 as one terminated bus. The host stages a frame into bus-0's
// downlink mailbox; the board transmits it on CAN0; CAN1 receives it and the
// board latches it into bus-1's uplink mailbox; the host reads it back.
//
// Build (raw ecrt, like the sibling reference tools -- not part of the CMake
// build): see the g++ line in the session notes; needs the IgH master running.
//   RMCS_ECAT_BACKEND is irrelevant here; the master is chosen via ethercat.conf.
//   sudo ./ecat_native_can_latency [samples] [cycle_core] [main_core]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

#include <ecrt.h>

#include <librmcs/ecat/native_can.hpp>

namespace native = librmcs::ecat;

namespace {

// Slave identity + PDO mapping, identical to host/src/transport/igh/igh.cpp.
constexpr uint32_t kVendorId = 0x00001A81;
constexpr uint32_t kProductCode = 0x00000001;
constexpr uint16_t kRxPdoIndex = 0x1600;
constexpr uint16_t kTxPdoIndex = 0x1a00;
constexpr uint16_t kOutputEntryIndex = 0x7010;
constexpr uint16_t kInputEntryIndex = 0x6000;
constexpr unsigned kPdoEntryCount = 12;
constexpr uint8_t kAlStateOp = 1u << 3;

constexpr uint32_t kCanId = 0x556;
constexpr uint8_t kPayloadSize = 8;
constexpr uint32_t kSamplesDefault = 5000;
constexpr uint32_t kWarmupSamples = 100;
constexpr auto kResponseTimeout = std::chrono::microseconds{500};
constexpr auto kEchoTimeout = std::chrono::milliseconds{20};
constexpr auto kOperationalTimeout = std::chrono::seconds{10};

// Downlink is bus 0 (CAN0 TX), the echo returns on bus 1 (CAN1 RX).
constexpr uint8_t kTxBus = 0;
constexpr uint8_t kRxBus = 1;

uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void put_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get_u32_le(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) | static_cast<uint32_t>(src[1]) << 8
         | static_cast<uint32_t>(src[2]) << 16 | static_cast<uint32_t>(src[3]) << 24;
}

void configure_thread(int core, int priority, const char* name) {
    if (name)
        (void)pthread_setname_np(pthread_self(), name);
    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        (void)sched_setaffinity(0, sizeof(set), &set);
    }
    sched_param parameter{};
    parameter.sched_priority = priority;
    (void)sched_setscheduler(0, SCHED_FIFO, &parameter);
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty())
        return 0.0;
    return sorted[static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1))];
}

// Shared between the cycle thread (owner of the ecrt process image) and the
// sender. Downlink is published under a spinlock-free latest-wins image;
// uplink is published with a seqlock so the sender reads a torn-free snapshot.
struct Shared {
    std::atomic<bool> stop{false};

    // Downlink: the sender writes the 48-byte image, the cycle thread copies it
    // into the domain outputs each cycle. A generation counter lets the cycle
    // thread copy a consistent image.
    std::atomic<uint32_t> down_gen{0};
    uint8_t down_image[native::kNativePdSize] = {};

    // Uplink: the cycle thread publishes the 48-byte input image via seqlock.
    std::atomic<uint32_t> up_gen{0};
    uint8_t up_image[native::kNativePdSize] = {};

    std::atomic<uint64_t> cycles{0};
    std::atomic<uint64_t> wc_errors{0};
};

} // namespace

int main(int argc, char** argv) {
    const uint32_t samples =
        argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : kSamplesDefault;
    const int cycle_core = argc > 2 ? std::atoi(argv[2]) : -1;
    const int main_core = argc > 3 ? std::atoi(argv[3]) : -1;

    ec_master_t* master = ecrt_request_master(0);
    if (!master) {
        fprintf(stderr, "ecrt_request_master(0) failed (is the ethercat service running?)\n");
        return 1;
    }
    ec_domain_t* domain = ecrt_master_create_domain(master);
    ec_slave_config_t* sc = ecrt_master_slave_config(master, 0, 0, kVendorId, kProductCode);
    if (!domain || !sc) {
        fprintf(stderr, "domain/slave_config failed\n");
        return 1;
    }

    ec_pdo_entry_info_t out_entries[kPdoEntryCount];
    ec_pdo_entry_info_t in_entries[kPdoEntryCount];
    for (unsigned i = 0; i < kPdoEntryCount; ++i) {
        out_entries[i] = {kOutputEntryIndex, static_cast<uint8_t>(i + 1), 32};
        in_entries[i] = {kInputEntryIndex, static_cast<uint8_t>(i + 1), 32};
    }
    ec_pdo_info_t pdo_out[] = {{kRxPdoIndex, kPdoEntryCount, out_entries}};
    ec_pdo_info_t pdo_in[] = {{kTxPdoIndex, kPdoEntryCount, in_entries}};
    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
        {1, EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, pdo_out, EC_WD_ENABLE},
        {3, EC_DIR_INPUT, 1, pdo_in, EC_WD_DISABLE},
        {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
    };
    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr, "ecrt_slave_config_pdos failed\n");
        return 1;
    }
    if (ecrt_master_select_reference_clock(master, nullptr)) {
        fprintf(stderr, "select_reference_clock(none) failed\n");
        return 1;
    }
    const int off_out = ecrt_slave_config_reg_pdo_entry(sc, kOutputEntryIndex, 1, domain, nullptr);
    const int off_in = ecrt_slave_config_reg_pdo_entry(sc, kInputEntryIndex, 1, domain, nullptr);
    if (off_out < 0 || off_in < 0) {
        fprintf(stderr, "reg_pdo_entry failed (out=%d in=%d)\n", off_out, off_in);
        return 1;
    }
    if (ecrt_master_activate(master)) {
        fprintf(stderr, "ecrt_master_activate failed\n");
        return 1;
    }
    uint8_t* process_data = ecrt_domain_data(domain);
    uint8_t* outputs = process_data + off_out;
    uint8_t* inputs = process_data + off_in;
    std::memset(outputs, 0, native::kNativePdSize);

    auto exchange_once = [&]() {
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        const auto deadline = std::chrono::steady_clock::now() + kResponseTimeout;
        ec_domain_state_t ds{};
        for (;;) {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            ecrt_domain_state(domain, &ds);
            if (ds.wc_state == EC_WC_COMPLETE || std::chrono::steady_clock::now() >= deadline)
                break;
        }
        return ds;
    };

    // Drive the FSM to OP.
    const auto op_deadline = std::chrono::steady_clock::now() + kOperationalTimeout;
    for (;;) {
        exchange_once();
        ec_master_state_t ms{};
        ecrt_master_state(master, &ms);
        if (ms.al_states & kAlStateOp)
            break;
        if (std::chrono::steady_clock::now() >= op_deadline) {
            fprintf(stderr, "slave did not reach OP\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    printf("native CAN mailbox link up: vendor 0x%08X product 0x%08X\n", kVendorId, kProductCode);

    Shared shared;

    // Cycle thread: owns the ecrt image. Each cycle it applies the sender's
    // latest downlink image and publishes the fresh uplink image (seqlock).
    std::thread cycle_thread{[&]() {
        configure_thread(cycle_core, 80, "native-cyc");
        uint32_t seen_down_gen = 0;
        uint8_t local_down[native::kNativePdSize] = {};
        while (!shared.stop.load(std::memory_order_relaxed)) {
            const uint32_t g = shared.down_gen.load(std::memory_order_acquire);
            if (g != seen_down_gen) {
                std::memcpy(local_down, shared.down_image, native::kNativePdSize);
                seen_down_gen = g;
            }
            std::memcpy(outputs, local_down, native::kNativePdSize);

            const ec_domain_state_t ds = exchange_once();
            shared.cycles.fetch_add(1, std::memory_order_relaxed);
            if (ds.wc_state != EC_WC_COMPLETE)
                shared.wc_errors.fetch_add(1, std::memory_order_relaxed);

            const uint32_t seq = shared.up_gen.load(std::memory_order_relaxed);
            shared.up_gen.store(seq + 1, std::memory_order_release); // odd: writing
            std::memcpy(shared.up_image, inputs, native::kNativePdSize);
            shared.up_gen.store(seq + 2, std::memory_order_release); // even: stable
        }
    }};

    configure_thread(main_core, 70, "native-main");

    std::vector<double> rtts_us;
    rtts_us.reserve(samples);
    uint32_t timeouts = 0;
    uint8_t mailbox_seq = 0;

    using Clock = std::chrono::steady_clock;
    for (uint32_t sequence = 0; sequence < samples + kWarmupSamples; ++sequence) {
        uint8_t payload[kPayloadSize];
        put_u32_le(payload, sequence);
        put_u32_le(payload + 4, mix(sequence));

        // Stage the downlink mailbox for bus 0.
        mailbox_seq = mailbox_seq == 255 ? 1 : static_cast<uint8_t>(mailbox_seq + 1);
        uint8_t image[native::kNativePdSize] = {};
        uint8_t* mb = image + kTxBus * native::kNativeMailboxSize;
        mb[native::kNativeSeqOffset] = mailbox_seq;
        mb[native::kNativeMetaOffset] = native::native_meta(true, kPayloadSize);
        mb[native::kNativeIdOffset] = static_cast<uint8_t>(kCanId);
        mb[native::kNativeIdOffset + 1] = static_cast<uint8_t>(kCanId >> 8);
        std::memcpy(mb + native::kNativeDataOffset, payload, kPayloadSize);

        const uint32_t g = shared.down_gen.load(std::memory_order_relaxed);
        std::memcpy(shared.down_image, image, native::kNativePdSize);
        shared.down_gen.store(g + 1, std::memory_order_release);
        const auto send_time = Clock::now();

        // Busy-wait for bus-1's uplink mailbox to carry our exact payload.
        bool matched = false;
        const auto deadline = Clock::now() + kEchoTimeout;
        while (Clock::now() < deadline) {
            uint32_t g1 = shared.up_gen.load(std::memory_order_acquire);
            if (g1 & 1u)
                continue;
            uint8_t snapshot[native::kNativePdSize];
            std::memcpy(snapshot, shared.up_image, native::kNativePdSize);
            const uint32_t g2 = shared.up_gen.load(std::memory_order_acquire);
            if (g1 != g2)
                continue;
            const uint8_t* rx = snapshot + kRxBus * native::kNativeMailboxSize;
            if (native::native_meta_is_fdcan(rx[native::kNativeMetaOffset])
                && native::native_meta_length(rx[native::kNativeMetaOffset]) == kPayloadSize
                && get_u32_le(rx + native::kNativeDataOffset) == sequence
                && get_u32_le(rx + native::kNativeDataOffset + 4) == mix(sequence)) {
                const double rtt_us =
                    std::chrono::duration<double, std::micro>(Clock::now() - send_time).count();
                if (sequence >= kWarmupSamples)
                    rtts_us.push_back(rtt_us);
                matched = true;
                break;
            }
        }
        if (!matched && sequence >= kWarmupSamples)
            ++timeouts;
    }

    shared.stop.store(true, std::memory_order_relaxed);
    cycle_thread.join();

    if (rtts_us.empty()) {
        fprintf(stderr, "no valid loopback frames (check CAN0<->CAN1 wiring + native firmware)\n");
        ecrt_master_deactivate(master);
        ecrt_release_master(master);
        return 2;
    }

    std::sort(rtts_us.begin(), rtts_us.end());
    double sum = 0.0;
    for (double v : rtts_us)
        sum += v;
    printf(
        "native CAN: %zu round trips, timeout=%u, %llu cycles, %llu wc errors\n", rtts_us.size(),
        timeouts, static_cast<unsigned long long>(shared.cycles.load()),
        static_cast<unsigned long long>(shared.wc_errors.load()));
    printf(
        "rtt us: min %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  avg %.1f  max %.1f\n",
        rtts_us.front(), percentile(rtts_us, 0.50), percentile(rtts_us, 0.90),
        percentile(rtts_us, 0.99), percentile(rtts_us, 0.999),
        sum / static_cast<double>(rtts_us.size()), rtts_us.back());

    ecrt_master_deactivate(master);
    ecrt_release_master(master);
    return 0;
}
