// Reference only -- NOT built by CMake, not part of the librmcs build.
//
// This is the exact, working standalone program used to prove out the IgH
// EtherCAT Master native igc driver against the rmcs_board slave before
// committing to writing the real host/src/transport/igh/igh.cpp backend.
// Every ecrt call sequence here (including the distributed-clock trio that
// is easy to miss, see DESIGN.md) was verified against real hardware.
//
// To reproduce: after `sudo ethercatctl start` and after installing the IgH
// master per DESIGN.md's "prerequisites" section, run (as one line):
//   g++ -O2 -std=c++20 -Wall -I/usr/local/include igh_latency_bench.cpp
//     -o igh_latency_bench -L/usr/local/lib -lethercat -lpthread -lrt
//   sudo taskset -c 6 ./igh_latency_bench 10
//
// Measured on TL101 (igc i225/i226, kernel 6.8.1-1015-realtime):
//   cycles ~221000  timeouts ~7  cycle rate ~22.1 kHz
//   rtt us: p50 ~43  p90 ~44.5  p99 ~50  (max is an occasional multi-ms
//   scheduling-hiccup outlier, seen across every transport tested, not
//   specific to this driver -- see EVALUATION.md)
//
// PDO layout comes straight from `ethercat pdos -p 0` against the live
// slave: SM2 = RxPDO 0x1600 (master->slave, 32 x 32-bit at 0x7010:01..20,
// 128 bytes), SM3 = TxPDO 0x1a00 (slave->master, 32 x 32-bit at
// 0x6000:01..20, 128 bytes). The slave reports "PDO Assign: no" / "PDO
// Configuration: no" (fixed mapping), so ecrt_slave_config_pdos() is only
// used to describe the existing layout, not to change it.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <ecrt.h>

namespace {

constexpr uint32_t kVendorId = 0x00000511;
constexpr uint32_t kProductCode = 0x00000001;
constexpr int kChunkBytes = 128;
constexpr int kCpu = 7; // isolated core (isolcpus/nohz_full/rcu_nocbs=7)

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

double ts_to_us(const timespec &a, const timespec &b) {
    return (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
}

uint64_t ts_to_ns(const timespec &t) {
    return static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(t.tv_nsec);
}

ec_pdo_entry_info_t g_out_entries[32];
ec_pdo_entry_info_t g_in_entries[32];

void fill_entries(ec_pdo_entry_info_t *entries, uint16_t index) {
    for (int i = 0; i < 32; i++) {
        entries[i] = {index, static_cast<uint8_t>(i + 1), 32};
    }
}

} // namespace

int main(int argc, char **argv) {
    const double duration_s = argc > 1 ? std::atof(argv[1]) : 10.0;
    signal(SIGINT, on_sigint);

    ec_master_t *master = ecrt_request_master(0);
    if (!master) {
        std::fprintf(stderr, "ecrt_request_master failed (is the ethercat service running?)\n");
        return 1;
    }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) {
        std::fprintf(stderr, "ecrt_master_create_domain failed\n");
        return 1;
    }

    ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, 0, kVendorId, kProductCode);
    if (!sc) {
        std::fprintf(stderr, "ecrt_master_slave_config failed\n");
        return 1;
    }

    fill_entries(g_out_entries, 0x7010);
    fill_entries(g_in_entries, 0x6000);

    ec_pdo_info_t pdo_out[] = {{0x1600, 32, g_out_entries}};
    ec_pdo_info_t pdo_in[] = {{0x1a00, 32, g_in_entries}};
    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
        {1, EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, pdo_out, EC_WD_ENABLE},
        {3, EC_DIR_INPUT, 1, pdo_in, EC_WD_DISABLE},
        {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
    };

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        std::fprintf(stderr, "ecrt_slave_config_pdos failed\n");
        return 1;
    }

    const int off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7010, 1, domain, nullptr);
    const int off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 1, domain, nullptr);
    if (off_out < 0 || off_in < 0) {
        std::fprintf(stderr, "ecrt_slave_config_reg_pdo_entry failed (out=%d in=%d)\n", off_out,
                     off_in);
        return 1;
    }

    if (ecrt_master_activate(master)) {
        std::fprintf(stderr, "ecrt_master_activate failed\n");
        return 1;
    }

    uint8_t *pd = ecrt_domain_data(domain);
    if (!pd) {
        std::fprintf(stderr, "ecrt_domain_data failed\n");
        return 1;
    }

    // Pin + realtime priority, matching the SOEM cycle thread setup.
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(kCpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    sched_param sp{};
    sp.sched_priority = 80;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    std::fprintf(stderr, "cycle thread: SCHED_FIFO priority 80, pinned to CPU %d\n", kCpu);

    // Warm-up: drive the master's internal FSM until all slaves report OP.
    // The distributed-clock calls below (application_time / sync_reference_
    // clock_to / sync_slave_clocks) are required every cycle because this
    // slave is auto-elected as the DC reference clock -- without them the
    // slave hangs in PREOP or SAFEOP+ERROR forever (see DESIGN.md).
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
        if (ms.link_up && ms.slaves_responding && (ms.al_states & (1u << 3))) {
            break;
        }

        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_to_us(warmup_start, now) > 5'000'000.0) {
            std::fprintf(stderr, "timed out waiting for OP (al_states=0x%x)\n", ms.al_states);
            return 1;
        }
        usleep(1000);
    }
    std::fprintf(stderr, "slave(s) in OP, starting %.1fs latency run (1 in flight)...\n",
                 duration_s);

    std::vector<double> rtt_us;
    rtt_us.reserve(200000);
    unsigned timeouts = 0;
    uint32_t counter = 0;

    timespec run_start;
    clock_gettime(CLOCK_MONOTONIC, &run_start);
    timespec now;
    do {
        std::memcpy(pd + off_out, &counter, sizeof(counter));
        counter++;

        timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ecrt_master_application_time(master, ts_to_ns(t0));
        ecrt_master_sync_reference_clock_to(master, ts_to_ns(t0));
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
            if (ts_to_us(t0, t1) > 5000.0) { // 5ms: treat as a dropped cycle
                timeouts++;
                break;
            }
        }
        if (ds.wc_state == EC_WC_COMPLETE)
            rtt_us.push_back(ts_to_us(t0, t1));

        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!g_stop && ts_to_us(run_start, now) < duration_s * 1e6);

    ecrt_master_deactivate(master);
    ecrt_release_master(master);

    if (rtt_us.empty()) {
        std::fprintf(stderr, "no successful cycles\n");
        return 1;
    }
    std::sort(rtt_us.begin(), rtt_us.end());
    auto pct = [&](double p) { return rtt_us[static_cast<size_t>(p * (rtt_us.size() - 1))]; };
    const double cycle_rate_khz = rtt_us.size() / (ts_to_us(run_start, now) / 1e6) / 1000.0;

    std::fprintf(stderr,
                 "cycles %zu  timeouts %u  cycle rate %.1f kHz\n"
                 "rtt us: p50 %.1f  p90 %.1f  p99 %.1f  max %.1f  (n=%zu)\n",
                 rtt_us.size(), timeouts, cycle_rate_khz, pct(0.50), pct(0.90), pct(0.99),
                 rtt_us.back(), rtt_us.size());
    return 0;
}
