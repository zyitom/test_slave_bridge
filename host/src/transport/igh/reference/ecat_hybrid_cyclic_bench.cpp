// Phase-locked 4 x 7 CAN control benchmark for RMCS_ECAT_HYBRID_PD.
//
// The 352-byte PDO contains 28 fixed CAN slots (seven per bus) followed by a
// 16-byte reliable stream chunk. One thread stages all 28 commands immediately
// before the EtherCAT send that carries them, removing the 0..Tpdo sampling
// phase of an asynchronous application thread. This is the recommended
// measurement path for cyclic-control jitter; the normal SDK
// start_cyclic_transmit() API removes stream queueing but remains asynchronous
// to the master cycle unless called from a transport callback.
//
// Wire CAN1 <-> CAN2 and CAN3 <-> CAN4 as two terminated buses. Every controller
// queues seven CAN-FD frames per tick. Its paired controller receives those
// frames, so all 28 commands return through the 28 rotating uplink slots.
//
// Build from the repository root:
//   g++ -O2 -std=c++20 -I/usr/local/include -Icore/include
//     host/src/transport/igh/reference/ecat_hybrid_cyclic_bench.cpp -o
//     ecat_hybrid_cyclic_bench -L/usr/local/lib -lethercat -lpthread -lrt
// Run with the IgH master active:
//   sudo ./ecat_hybrid_cyclic_bench [samples] [hz] [cycle_core]

#include <algorithm>
#include <array>
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

#include <librmcs/ecat/hybrid_pd.hpp>
#include <librmcs/ecat/native_can.hpp>

#include "hybrid_session.hpp"

namespace ecat = librmcs::ecat;
namespace reference = librmcs::host::transport::igh::reference;

namespace {

constexpr uint32_t kVendorId = 0x00001A81;
constexpr uint32_t kProductCode = 0x00000001;
constexpr uint16_t kRxPdoIndex = 0x1600;
constexpr uint16_t kTxPdoIndex = 0x1A00;
constexpr uint16_t kOutFixedIndex = 0x7000;
constexpr uint16_t kOutStreamIndex = 0x7010;
constexpr uint16_t kInFixedIndex = 0x6000;
constexpr uint16_t kInStreamIndex = 0x6010;
constexpr unsigned kFixedEntryCount = ecat::kHybridMailboxRegionSize / sizeof(uint32_t);
constexpr unsigned kStreamEntryCount = ecat::kHybridStreamChunkSize / sizeof(uint32_t);
constexpr unsigned kPdoEntryCount = kFixedEntryCount + kStreamEntryCount;
constexpr uint8_t kAlStateOp = 1U << 3;

constexpr uint32_t kSamplesDefault = 5000;
constexpr uint32_t kHzDefault = 1000;
constexpr uint32_t kWarmupTicks = 100;
constexpr uint8_t kPayloadSize = 8;
constexpr uint32_t kCanIdBase = 0x500;
constexpr auto kResponseTimeout = std::chrono::microseconds{500};
constexpr auto kOperationalTimeout = std::chrono::seconds{10};
constexpr auto kSessionTimeout = std::chrono::seconds{1};
constexpr auto kFinalTickTimeout = std::chrono::milliseconds{20};

static_assert(kFixedEntryCount == 84);
static_assert(kPdoEntryCount == 88);

using Clock = std::chrono::steady_clock;

void put_u32_le(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get_u32_le(const uint8_t* source) {
    return static_cast<uint32_t>(source[0]) | static_cast<uint32_t>(source[1]) << 8
         | static_cast<uint32_t>(source[2]) << 16 | static_cast<uint32_t>(source[3]) << 24;
}

uint8_t payload_check(uint32_t tick, uint8_t bus, uint8_t slot) {
    return static_cast<uint8_t>(tick ^ (tick >> 8) ^ (tick >> 16) ^ (tick >> 24)
                                ^ (static_cast<uint32_t>(bus) << 4) ^ slot ^ 0xA5U);
}

uint32_t can_id(uint8_t bus, uint8_t slot) {
    return kCanIdBase + static_cast<uint32_t>(bus) * 16U + slot;
}

void configure_thread(int core) {
    (void)pthread_setname_np(pthread_self(), "hybrid-28-cyc");
    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            perror("sched_setaffinity");
    }
    sched_param parameter{};
    parameter.sched_priority = 80;
    if (sched_setscheduler(0, SCHED_FIFO, &parameter) != 0)
        perror("sched_setscheduler");
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty())
        return 0.0;
    return sorted[static_cast<std::size_t>(fraction * (sorted.size() - 1))];
}

void print_distribution(const char* label, std::vector<double>& values) {
    if (values.empty()) {
        printf("%s: no samples\n", label);
        return;
    }
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (double value : values)
        sum += value;
    const double p50 = percentile(values, 0.50);
    const double p99 = percentile(values, 0.99);
    const double p999 = percentile(values, 0.999);
    printf(
        "%s us: min %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  avg %.1f  max %.1f\n",
        label, values.front(), p50, percentile(values, 0.90), p99, p999,
        sum / static_cast<double>(values.size()), values.back());
    printf(
        "%s jitter us: p99-p50 %.1f  p99.9-p50 %.1f  max-p50 %.1f\n", label,
        p99 - p50, p999 - p50, values.back() - p50);
}

struct PendingTick {
    bool active = false;
    bool measured = false;
    uint32_t tick = 0;
    Clock::time_point staged_at{};
    std::array<bool, ecat::kHybridMailboxCount> seen{};
    std::size_t seen_count = 0;
};

} // namespace

int main(int argc, char** argv) {
    const uint32_t samples =
        argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : kSamplesDefault;
    const uint32_t hz =
        argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)) : kHzDefault;
    const int cycle_core = argc > 3 ? std::atoi(argv[3]) : -1;
    if (samples == 0 || hz == 0) {
        fprintf(stderr, "samples and hz must be positive\n");
        return 1;
    }

    ec_master_t* master = ecrt_request_master(0);
    if (!master) {
        fprintf(stderr, "ecrt_request_master(0) failed (is the EtherCAT service running?)\n");
        return 1;
    }
    ec_domain_t* domain = ecrt_master_create_domain(master);
    ec_slave_config_t* slave =
        ecrt_master_slave_config(master, 0, 0, kVendorId, kProductCode);
    if (!domain || !slave) {
        fprintf(stderr, "domain/slave configuration failed\n");
        ecrt_release_master(master);
        return 1;
    }

    std::array<ec_pdo_entry_info_t, kPdoEntryCount> output_entries{};
    std::array<ec_pdo_entry_info_t, kPdoEntryCount> input_entries{};
    for (unsigned index = 0; index < kFixedEntryCount; ++index) {
        output_entries[index] = {
            kOutFixedIndex, static_cast<uint8_t>(index + 1), 32};
        input_entries[index] = {
            kInFixedIndex, static_cast<uint8_t>(index + 1), 32};
    }
    for (unsigned index = 0; index < kStreamEntryCount; ++index) {
        output_entries[kFixedEntryCount + index] = {
            kOutStreamIndex, static_cast<uint8_t>(index + 1), 32};
        input_entries[kFixedEntryCount + index] = {
            kInStreamIndex, static_cast<uint8_t>(index + 1), 32};
    }
    ec_pdo_info_t output_pdo[] = {
        {kRxPdoIndex, kPdoEntryCount, output_entries.data()}
    };
    ec_pdo_info_t input_pdo[] = {
        {kTxPdoIndex, kPdoEntryCount, input_entries.data()}
    };
    ec_sync_info_t syncs[] = {
        {   0,  EC_DIR_OUTPUT, 0,      nullptr, EC_WD_DISABLE},
        {   1,   EC_DIR_INPUT, 0,      nullptr, EC_WD_DISABLE},
        {   2,  EC_DIR_OUTPUT, 1,   output_pdo,  EC_WD_ENABLE},
        {   3,   EC_DIR_INPUT, 1,    input_pdo, EC_WD_DISABLE},
        {0xFF, EC_DIR_INVALID, 0,      nullptr, EC_WD_DEFAULT},
    };
    if (ecrt_slave_config_pdos(slave, EC_END, syncs)
        || ecrt_master_select_reference_clock(master, nullptr)) {
        fprintf(stderr, "PDO or reference-clock configuration failed\n");
        ecrt_release_master(master);
        return 1;
    }
    const int output_offset =
        ecrt_slave_config_reg_pdo_entry(slave, kOutFixedIndex, 1, domain, nullptr);
    const int input_offset =
        ecrt_slave_config_reg_pdo_entry(slave, kInFixedIndex, 1, domain, nullptr);
    if (output_offset < 0 || input_offset < 0 || ecrt_master_activate(master)) {
        fprintf(stderr, "PDO registration or master activation failed\n");
        ecrt_release_master(master);
        return 1;
    }

    uint8_t* process_data = ecrt_domain_data(domain);
    uint8_t* outputs = process_data + output_offset;
    const uint8_t* inputs = process_data + input_offset;
    std::memset(outputs, 0, ecat::kHybridPdSize);
    reference::HybridSession session;

    auto exchange_once = [&]() {
        session.prepare_output(outputs, Clock::now());
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        const auto deadline = Clock::now() + kResponseTimeout;
        ec_domain_state_t state{};
        do {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            ecrt_domain_state(domain, &state);
        } while (state.wc_state != EC_WC_COMPLETE && Clock::now() < deadline);
        if (state.wc_state == EC_WC_COMPLETE)
            session.consume_input(inputs, Clock::now());
        return state;
    };

    const auto operational_deadline = Clock::now() + kOperationalTimeout;
    for (;;) {
        exchange_once();
        ec_master_state_t state{};
        ecrt_master_state(master, &state);
        if (state.al_states & kAlStateOp)
            break;
        if (Clock::now() >= operational_deadline) {
            fprintf(stderr, "slave did not reach OP (flash the 352-byte hybrid firmware)\n");
            ecrt_master_deactivate(master);
            ecrt_release_master(master);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    session.begin();
    const auto session_deadline = Clock::now() + kSessionTimeout;
    while (!session.established()) {
        exchange_once();
        if (Clock::now() >= session_deadline) {
            fprintf(stderr, "timed out waiting for librmcs SESSION_START_ACK\n");
            ecrt_master_deactivate(master);
            ecrt_release_master(master);
            return 1;
        }
    }

    configure_thread(cycle_core);
    printf(
        "hybrid 28-slot link up: %u Hz, CAN1<->CAN2 and CAN3<->CAN4, %u measured ticks\n",
        hz, samples);

    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>{1.0 / static_cast<double>(hz)});
    const uint32_t total_ticks = samples + kWarmupTicks;
    std::array<uint8_t, ecat::kHybridMailboxCount> output_seq{};
    std::array<uint8_t, ecat::kHybridMailboxCount> input_seq{};
    PendingTick pending;

    std::vector<double> frame_rtt_us;
    frame_rtt_us.reserve(static_cast<std::size_t>(samples) * ecat::kHybridMailboxCount);
    std::vector<double> tick_rtt_us;
    tick_rtt_us.reserve(samples);
    std::vector<double> tick_period_us;
    tick_period_us.reserve(samples);
    std::vector<double> cycle_period_us;
    cycle_period_us.reserve(static_cast<std::size_t>(samples) * 16);

    uint32_t staged_ticks = 0;
    uint32_t missed_ticks = 0;
    uint64_t cycles = 0;
    uint64_t wc_errors = 0;
    Clock::time_point next_tick = Clock::now() + period;
    Clock::time_point last_tick{};
    Clock::time_point last_cycle = Clock::now();

    auto stage_tick = [&](uint32_t tick, Clock::time_point now) {
        for (uint8_t bus = 0; bus < ecat::kNativeBusCount; ++bus) {
            for (uint8_t slot = 0; slot < ecat::kHybridSlotsPerBus; ++slot) {
                const std::size_t mailbox_index = ecat::hybrid_mailbox_index(bus, slot);
                uint8_t* mailbox =
                    outputs + ecat::hybrid_mailbox_offset(bus, slot);
                output_seq[mailbox_index] =
                    output_seq[mailbox_index] == 255
                        ? 1
                        : static_cast<uint8_t>(output_seq[mailbox_index] + 1);
                mailbox[ecat::kNativeSeqOffset] = output_seq[mailbox_index];
                mailbox[ecat::kNativeMetaOffset] = ecat::native_meta(true, kPayloadSize);
                const uint32_t id = can_id(bus, slot);
                mailbox[ecat::kNativeIdOffset] = static_cast<uint8_t>(id);
                mailbox[ecat::kNativeIdOffset + 1] = static_cast<uint8_t>(id >> 8);
                uint8_t* payload = mailbox + ecat::kNativeDataOffset;
                put_u32_le(payload, tick);
                payload[4] = bus;
                payload[5] = slot;
                payload[6] = payload_check(tick, bus, slot);
                payload[7] = static_cast<uint8_t>(~payload[6]);
            }
        }
        pending = {};
        pending.active = true;
        pending.measured = tick >= kWarmupTicks;
        pending.tick = tick;
        pending.staged_at = now;
    };

    auto finish_missed_tick = [&]() {
        if (pending.active && pending.measured)
            ++missed_ticks;
        pending.active = false;
    };

    while (staged_ticks < total_ticks || pending.active) {
        const auto now = Clock::now();
        if (staged_ticks < total_ticks && now >= next_tick) {
            finish_missed_tick();
            if (staged_ticks > kWarmupTicks) {
                tick_period_us.push_back(
                    std::chrono::duration<double, std::micro>(now - last_tick).count());
            }
            last_tick = now;
            stage_tick(staged_ticks, now);
            ++staged_ticks;
            next_tick += period;
            if (next_tick < Clock::now())
                next_tick = Clock::now() + period;
        }

        const ec_domain_state_t domain_state = exchange_once();
        const auto cycle_now = Clock::now();
        if (cycles != 0 && staged_ticks > kWarmupTicks) {
            cycle_period_us.push_back(
                std::chrono::duration<double, std::micro>(cycle_now - last_cycle).count());
        }
        last_cycle = cycle_now;
        ++cycles;
        if (domain_state.wc_state != EC_WC_COMPLETE)
            ++wc_errors;

        for (std::size_t input_slot = 0; input_slot < ecat::kHybridMailboxCount;
             ++input_slot) {
            const uint8_t* mailbox = inputs + input_slot * ecat::kNativeMailboxSize;
            const uint8_t seq = mailbox[ecat::kNativeSeqOffset];
            if (seq == 0 || seq == input_seq[input_slot])
                continue;
            input_seq[input_slot] = seq;
            if (!pending.active
                || ecat::native_meta_length(mailbox[ecat::kNativeMetaOffset]) != kPayloadSize) {
                continue;
            }

            const uint8_t* payload = mailbox + ecat::kNativeDataOffset;
            const uint32_t tick = get_u32_le(payload);
            const uint8_t tx_bus = payload[4];
            const uint8_t tx_slot = payload[5];
            const std::size_t rx_bus = input_slot / ecat::kHybridSlotsPerBus;
            if (tick != pending.tick || tx_bus >= ecat::kNativeBusCount
                || tx_slot >= ecat::kHybridSlotsPerBus || rx_bus != (tx_bus ^ 1U)
                || payload[6] != payload_check(tick, tx_bus, tx_slot)
                || payload[7] != static_cast<uint8_t>(~payload[6])) {
                continue;
            }

            const std::size_t command_index = ecat::hybrid_mailbox_index(tx_bus, tx_slot);
            if (pending.seen[command_index])
                continue;
            pending.seen[command_index] = true;
            ++pending.seen_count;
            if (pending.measured) {
                frame_rtt_us.push_back(
                    std::chrono::duration<double, std::micro>(cycle_now - pending.staged_at)
                        .count());
            }
            if (pending.seen_count == ecat::kHybridMailboxCount) {
                if (pending.measured) {
                    tick_rtt_us.push_back(
                        std::chrono::duration<double, std::micro>(cycle_now - pending.staged_at)
                            .count());
                }
                pending.active = false;
            }
        }

        if (staged_ticks == total_ticks && pending.active
            && cycle_now - pending.staged_at >= kFinalTickTimeout) {
            finish_missed_tick();
        }
    }

    printf(
        "ticks complete=%zu missed=%u, frames=%zu/%zu, cycles=%llu, wc errors=%llu\n",
        tick_rtt_us.size(), missed_ticks, frame_rtt_us.size(),
        static_cast<std::size_t>(samples) * ecat::kHybridMailboxCount,
        static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(wc_errors));
    print_distribution("frame rtt", frame_rtt_us);
    print_distribution("full tick rtt", tick_rtt_us);
    print_distribution("control tick period", tick_period_us);
    print_distribution("EtherCAT cycle period", cycle_period_us);

    ecrt_master_deactivate(master);
    ecrt_release_master(master);
    return missed_ticks == 0 && tick_rtt_us.size() == samples && wc_errors == 0 ? 0 : 2;
}
