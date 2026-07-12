// Reference only -- NOT built by CMake, not part of the librmcs build.
//
// A no-ARQ demo: it drives the EtherCAT process data DIRECTLY (raw
// latest-value PDO), with NO PdStreamEndpoint / stop-and-wait ARQ at all, and
// measures what removing the ARQ actually buys and costs:
//
//   1. loopback DEPTH in cycles  -- how many EtherCAT frames pass between
//      writing a counter into the output PDO and reading it back from the
//      input PDO. Compare against the ARQ stream transport's ~4 cycles.
//   2. round-trip TIME in us     -- a send timestamp is written into the PDO
//      and echoed back, so RTT = now - echoed_ts is a single-clock measurement
//      (no host/slave clock sync needed).
//   3. drop / dup / reorder count -- with no ARQ, EtherCAT's SyncManager
//      "3-buffer latest-wins" semantics DROP or DUPLICATE whole chunks when
//      the poll rate and the slave update rate disagree. This counts exactly
//      the events the ARQ exists to hide. This is the price of best-effort.
//
// IMPORTANT: this requires the SLAVE to be running a RAW passthrough firmware
// that copies the output PDO straight back into the input PDO (optionally via
// core1), WITHOUT the ARQ endpoint in pd_glue.cpp. Against the stock ARQ
// firmware the echoed bytes are ARQ frames, not our raw counter, so the depth /
// RTT / drop stats will be meaningless (echoed_seq will look like garbage). The
// cycle rate line is still valid either way and already answers "does removing
// ARQ change the frequency" (it does not -- the frame period is set by the
// wire + slave, not by what rides in the PDO).
//
// Build (same as igh_latency_bench, one line):
//   g++ -O2 -std=c++20 -Wall -I/usr/local/include igh_noarq_demo.cpp
//     -o igh_noarq_demo -L/usr/local/lib -lethercat -lpthread -lrt
//   sudo taskset -c 7 ./igh_noarq_demo 10

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <signal.h>
#include <vector>

#include <ecrt.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {

constexpr uint32_t kVendorId = 0x00000511;
constexpr uint32_t kProductCode = 0x00000001;
constexpr int kChunkBytes = 48;
constexpr int kPdoEntryCount = 12;
constexpr int kCpu = 7; // isolated core (isolcpus/nohz_full/rcu_nocbs=7)

// Layout we stamp into the raw output PDO (and expect echoed back verbatim):
//   [0..4)   uint32 sequence counter (incremented every cycle)
//   [4..12)  uint64 send timestamp in ns
// A never-sent sentinel lets us ignore the input image before the first of our
// own frames has come back.
constexpr uint32_t kSeqNeverSent = 0xFFFFFFFFu;

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

double ts_to_us(const timespec& a, const timespec& b) {
    return (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
}

uint64_t ts_to_ns(const timespec& t) {
    return static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(t.tv_nsec);
}

ec_pdo_entry_info_t g_out_entries[kPdoEntryCount];
ec_pdo_entry_info_t g_in_entries[kPdoEntryCount];

void fill_entries(ec_pdo_entry_info_t* entries, uint16_t index) {
    for (int i = 0; i < kPdoEntryCount; i++) {
        entries[i] = {index, static_cast<uint8_t>(i + 1), 32};
    }
}

} // namespace

int main(int argc, char** argv) {
    const double duration_s = argc > 1 ? std::atof(argv[1]) : 10.0;
    signal(SIGINT, on_sigint);

    ec_master_t* master = ecrt_request_master(0);
    if (!master) {
        std::fprintf(stderr, "ecrt_request_master failed (is the ethercat service running?)\n");
        return 1;
    }
    ec_domain_t* domain = ecrt_master_create_domain(master);
    ec_slave_config_t* sc = ecrt_master_slave_config(master, 0, 0, kVendorId, kProductCode);
    if (!domain || !sc) {
        std::fprintf(stderr, "domain / slave_config failed\n");
        return 1;
    }

    fill_entries(g_out_entries, 0x7010);
    fill_entries(g_in_entries, 0x6000);
    ec_pdo_info_t pdo_out[] = {
        {0x1600, kPdoEntryCount, g_out_entries}
    };
    ec_pdo_info_t pdo_in[] = {
        {0x1a00, kPdoEntryCount, g_in_entries}
    };
    ec_sync_info_t syncs[] = {
        {   0,  EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
        {   1,   EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
        {   2,  EC_DIR_OUTPUT, 1, pdo_out,  EC_WD_ENABLE},
        {   3,   EC_DIR_INPUT, 1,  pdo_in, EC_WD_DISABLE},
        {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
    };
    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        std::fprintf(stderr, "ecrt_slave_config_pdos failed\n");
        return 1;
    }
    const int off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7010, 1, domain, nullptr);
    const int off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 1, domain, nullptr);
    if (off_out < 0 || off_in < 0 || ecrt_master_activate(master)) {
        std::fprintf(stderr, "reg_pdo_entry / activate failed\n");
        return 1;
    }
    uint8_t* pd = ecrt_domain_data(domain);
    if (!pd) {
        std::fprintf(stderr, "ecrt_domain_data failed\n");
        return 1;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(kCpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    sched_param sp{};
    sp.sched_priority = 80;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    std::fprintf(stderr, "cycle thread: SCHED_FIFO priority 80, pinned to CPU %d\n", kCpu);

    // Warm-up to OP (distributed-clock trio every cycle, see igh_latency_bench).
    std::fprintf(stderr, "waiting for OP...\n");
    timespec warmup_start;
    clock_gettime(CLOCK_MONOTONIC, &warmup_start);
    for (;;) {
        timespec app_time;
        clock_gettime(CLOCK_MONOTONIC, &app_time);
        ecrt_master_application_time(master, ts_to_ns(app_time));
        std::memset(pd + off_out, 0, kChunkBytes);
        ecrt_master_sync_reference_clock_to(master, ts_to_ns(app_time));
        ecrt_master_sync_slave_clocks(master);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        ec_master_state_t ms;
        ecrt_master_state(master, &ms);
        if (ms.link_up && ms.slaves_responding && (ms.al_states & (1u << 3)))
            break;
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_to_us(warmup_start, now) > 5'000'000.0) {
            std::fprintf(stderr, "timed out waiting for OP (al_states=0x%x)\n", ms.al_states);
            return 1;
        }
        usleep(1000);
    }
    std::fprintf(stderr, "slave in OP, running %.1fs RAW (no-ARQ) loopback...\n", duration_s);

    std::vector<double> rtt_us;
    std::vector<uint32_t> depth_cycles;
    rtt_us.reserve(300000);
    depth_cycles.reserve(300000);
    uint64_t cycles = 0, echoes = 0, drops_or_dups = 0, reorders = 0, timeouts = 0;
    uint32_t seq = 0;
    uint32_t last_echoed = kSeqNeverSent;
    bool have_last = false;

    timespec run_start;
    clock_gettime(CLOCK_MONOTONIC, &run_start);
    timespec now;
    do {
        // Stamp a fresh sequence + send timestamp into the RAW output PDO. No
        // ARQ header, no ack tracking -- just the latest value, every cycle.
        timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        std::memset(pd + off_out, 0, kChunkBytes);
        std::memcpy(pd + off_out + 0, &seq, sizeof(seq));
        const uint64_t send_ns = ts_to_ns(t0);
        std::memcpy(pd + off_out + 4, &send_ns, sizeof(send_ns));

        ecrt_master_application_time(master, send_ns);
        ecrt_master_sync_reference_clock_to(master, send_ns);
        ecrt_master_sync_slave_clocks(master);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);

        ec_domain_state_t ds{};
        timespec t1;
        for (;;) {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            ecrt_domain_state(domain, &ds);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (ds.wc_state == EC_WC_COMPLETE)
                break;
            if (ts_to_us(t0, t1) > 5000.0) {
                timeouts++;
                break;
            }
        }
        cycles++;

        if (ds.wc_state == EC_WC_COMPLETE) {
            uint32_t echoed_seq = 0;
            uint64_t echoed_ns = 0;
            std::memcpy(&echoed_seq, pd + off_in + 0, sizeof(echoed_seq));
            std::memcpy(&echoed_ns, pd + off_in + 4, sizeof(echoed_ns));

            // Only trust a readback that really looks like one of our own
            // recent frames echoed back: sequence already sent, within a sane
            // depth window, and a plausible sub-100ms round trip. Against the
            // stock ARQ firmware the input image is an ARQ frame, so these
            // gates reject it (echoed_seq / echoed_ns are just header bytes) and
            // the "no valid echoes" guidance fires instead of printing noise.
            const double rtt = (static_cast<double>(send_ns - echoed_ns)) / 1e3;
            const bool plausible = echoed_seq != kSeqNeverSent && echoed_seq <= seq
                                && (seq - echoed_seq) < 10000u && rtt >= 0.0 && rtt < 100000.0;
            if (plausible) {
                echoes++;
                depth_cycles.push_back(seq - echoed_seq);
                rtt_us.push_back(rtt);

                if (have_last) {
                    if (echoed_seq == last_echoed)
                        drops_or_dups++; // same value twice -> a duplicate read
                    else if (echoed_seq < last_echoed)
                        reorders++;      // went backwards -> reorder (latest-wins race)
                    else if (echoed_seq > last_echoed + 1)
                        drops_or_dups += (echoed_seq - last_echoed - 1); // skipped -> drops
                }
                last_echoed = echoed_seq;
                have_last = true;
            }
        }
        seq++;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!g_stop && ts_to_us(run_start, now) < duration_s * 1e6);

    ecrt_master_deactivate(master);
    ecrt_release_master(master);

    const double elapsed_s = ts_to_us(run_start, now) / 1e6;
    const double cycle_rate_khz = cycles / elapsed_s / 1000.0;
    std::fprintf(stderr, "\n=== RAW (no-ARQ) loopback result ===\n");
    std::fprintf(
        stderr, "cycles %llu  cycle rate %.1f kHz  timeouts %llu\n",
        static_cast<unsigned long long>(cycles), cycle_rate_khz,
        static_cast<unsigned long long>(timeouts));

    if (echoes == 0) {
        std::fprintf(
            stderr,
            "NO valid echoes seen. The slave is almost certainly running the stock ARQ\n"
            "firmware (it framed our raw bytes as ARQ). Flash the RAW passthrough firmware\n"
            "to make depth/RTT/drop stats meaningful. (The cycle rate above is still valid\n"
            "and shows ARQ vs no-ARQ does not change the frequency.)\n");
        return 0;
    }

    std::sort(rtt_us.begin(), rtt_us.end());
    std::sort(depth_cycles.begin(), depth_cycles.end());
    auto pct = [](const auto& v, double p) {
        return v.empty() ? 0.0 : static_cast<double>(v[static_cast<size_t>(p * (v.size() - 1))]);
    };
    std::fprintf(
        stderr, "loopback depth (cycles): p50 %.0f  p90 %.0f  max %u  (compare ARQ ~4)\n",
        pct(depth_cycles, 0.50), pct(depth_cycles, 0.90), depth_cycles.back());
    std::fprintf(
        stderr, "round-trip us: p50 %.1f  p90 %.1f  p99 %.1f  max %.1f\n", pct(rtt_us, 0.50),
        pct(rtt_us, 0.90), pct(rtt_us, 0.99), rtt_us.empty() ? 0.0 : rtt_us.back());
    std::fprintf(
        stderr,
        "reliability cost of dropping ARQ: %llu drops/dups + %llu reorders over %llu "
        "echoes (%.3f%%)\n",
        static_cast<unsigned long long>(drops_or_dups), static_cast<unsigned long long>(reorders),
        static_cast<unsigned long long>(echoes),
        100.0 * static_cast<double>(drops_or_dups + reorders) / static_cast<double>(echoes));
    return 0;
}
