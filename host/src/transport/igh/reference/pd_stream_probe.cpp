// Live go-back-N trace: drives the real slave with the real PdStreamEndpoint
// and the exact ecat_stream_latency workload (64-byte echo frames, one in
// flight), tracing both chunk headers every cycle so the first corrupt
// delivery can be pinned to a wire-level event.
//
// Build (standalone, needs the installed IgH master, master must be started):
//   g++ -O2 -std=c++23 -Icore/include host/src/transport/igh/reference/pd_stream_probe.cpp \
//       -o pd_probe -I/usr/local/include -L/usr/local/lib -lethercat
// Run:   sudo ./pd_probe <cycles> <trace_cycles>          # echo workload
// Drain: sudo ./pd_probe <cycles> 0 drain                 # consume only, send nothing
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <span>
#include <thread>
#include <vector>

#include <ecrt.h>

#include <librmcs/ecat/pd_stream.hpp>

using librmcs::ecat::kPdChunkPayloadSize;
using librmcs::ecat::PdStreamEndpoint;

namespace {

constexpr std::size_t kFrameSize = 64;

struct ByteRing {
    std::deque<std::byte> q;
    bool try_push(std::span<const std::byte> data) {
        for (const std::byte b : data)
            q.push_back(b);
        return true;
    }
    std::size_t pop(std::span<std::byte> destination) {
        const std::size_t n = std::min(destination.size(), q.size());
        for (std::size_t i = 0; i < n; i++) {
            destination[i] = q.front();
            q.pop_front();
        }
        return n;
    }
};

struct FrameChecker {
    std::uint64_t expected_seq = 0;
    std::uint64_t frames = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t first_corrupt_frame = 0;
    std::vector<std::byte> reassembly;

    bool try_push(std::span<const std::byte> data) {
        for (const std::byte b : data) {
            reassembly.push_back(b);
            if (reassembly.size() == kFrameSize) {
                std::uint64_t seq = 0;
                std::memcpy(&seq, reassembly.data(), sizeof(seq));
                bool ok = seq == expected_seq;
                for (std::size_t i = 16; ok && i < kFrameSize; i++)
                    ok = reassembly[i] == static_cast<std::byte>(seq + i);
                frames++;
                if (!ok && corrupt++ == 0)
                    first_corrupt_frame = frames;
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
    const int cycles = argc > 1 ? std::atoi(argv[1]) : 2000;
    const int trace_cycles = argc > 2 ? std::atoi(argv[2]) : 100;

    ec_master_t* master = ecrt_request_master(0);
    if (!master)
        return 1;
    ec_domain_t* domain = ecrt_master_create_domain(master);
    ec_slave_config_t* sc = ecrt_master_slave_config(master, 0, 0, 0x00001A81, 0x00000001);

    ec_pdo_entry_info_t out_entries[12];
    ec_pdo_entry_info_t in_entries[12];
    for (unsigned i = 0; i < 12; i++) {
        out_entries[i] = {0x7010, static_cast<uint8_t>(i + 1), 32};
        in_entries[i] = {0x6000, static_cast<uint8_t>(i + 1), 32};
    }
    ec_pdo_info_t pdo_out[] = {
        {0x1600, 12, out_entries}
    };
    ec_pdo_info_t pdo_in[] = {
        {0x1a00, 12, in_entries}
    };
    ec_sync_info_t syncs[] = {
        {   0,  EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
        {   1,   EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
        {   2,  EC_DIR_OUTPUT, 1, pdo_out,  EC_WD_ENABLE},
        {   3,   EC_DIR_INPUT, 1,  pdo_in, EC_WD_DISABLE},
        {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
    };
    if (ecrt_slave_config_pdos(sc, EC_END, syncs))
        return 1;
    const int off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7010, 1, domain, nullptr);
    const int off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 1, domain, nullptr);
    if (off_out < 0 || off_in < 0)
        return 1;
    if (ecrt_master_select_reference_clock(master, nullptr))
        return 1;
    if (ecrt_master_activate(master))
        return 1;
    uint8_t* pd = ecrt_domain_data(domain);
    auto* out = reinterpret_cast<std::byte*>(pd + off_out);
    auto* in = reinterpret_cast<std::byte*>(pd + off_in);

    auto exchange = [&]() {
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        ec_domain_state_t ds{};
        for (;;) {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            ecrt_domain_state(domain, &ds);
            if (ds.wc_state == EC_WC_COMPLETE || std::chrono::steady_clock::now() >= deadline)
                return ds.wc_state == EC_WC_COMPLETE;
        }
    };

    std::memset(out, 0, 48);
    const auto op_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        exchange();
        ec_master_state_t ms{};
        ecrt_master_state(master, &ms);
        if (ms.link_up && ms.slaves_responding && (ms.al_states & 0x08))
            break;
        if (std::chrono::steady_clock::now() >= op_deadline) {
            std::fprintf(stderr, "no OP\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("OP reached\n");

    const bool drain_only = argc > 3; // any 3rd arg: consume + ack, never send

    PdStreamEndpoint endpoint;
    ByteRing tx_ring;
    FrameChecker checker;
    std::byte frame[kFrameSize];
    std::uint64_t send_seq = 0;
    if (!drain_only) {
        make_frame(send_seq++, frame);
        tx_ring.try_push({frame, kFrameSize});
    }

    endpoint.reset();
    std::memset(out, 0, 48);

    for (int n = 0; n < cycles; n++) {
        endpoint.build_own_chunk(out, tx_ring);

        uint16_t out_len = 0;
        std::memcpy(&out_len, out + 2, sizeof(out_len));
        const auto out_seq = static_cast<unsigned>(out[0]);
        const auto out_ack = static_cast<unsigned>(out[1]);

        const bool ok = exchange();
        const std::uint64_t frames_before = checker.frames;
        const std::uint64_t corrupt_before = checker.corrupt;
        if (ok)
            endpoint.on_peer_chunk(in, checker);

        if (n < trace_cycles || checker.corrupt != corrupt_before) {
            uint16_t in_len = 0;
            std::memcpy(&in_len, in + 2, sizeof(in_len));
            std::printf(
                "c%04d wc=%d OUT s=%3u a=%3u l=%2u | IN s=%3u a=%3u l=%2u | fr=%llu cor=%llu\n",
                n, ok, out_seq, out_ack, out_len, static_cast<unsigned>(in[0]),
                static_cast<unsigned>(in[1]), in_len,
                static_cast<unsigned long long>(checker.frames),
                static_cast<unsigned long long>(checker.corrupt));
        }
        if (!drain_only && checker.frames != frames_before) {
            make_frame(send_seq++, frame);
            tx_ring.try_push({frame, kFrameSize});
        }
    }

    std::printf(
        "total: frames %llu corrupt %llu (first corrupt at frame %llu), cycles %d\n",
        static_cast<unsigned long long>(checker.frames),
        static_cast<unsigned long long>(checker.corrupt),
        static_cast<unsigned long long>(checker.first_corrupt_frame), cycles);

    ecrt_master_deactivate(master);
    ecrt_release_master(master);
    return 0;
}
