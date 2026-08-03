// EtherCAT transport: carries the librmcs byte stream over the process data
// of the rmcs_board EtherCAT bridge (firmware/rmcs_board/ecat) using the IgH
// EtherCAT Master native driver stack (ecrt userspace API + in-kernel igc
// master module), as an alternative to the SOEM backend (soem.cpp).
//
// This is a second Transport implementation, not a replacement: it wins ~4x on
// round-trip latency by bypassing the whole socket/AF_XDP/BPF layer -- the
// master state machine runs in a kernel module that owns the NIC directly (see
// host/src/transport/igh/EVALUATION.md for the measured comparison). SOEM stays
// the portable default; IgH is opt-in for deployments that install the master.
//
// The whole file compiles away unless the build enables the optional IgH
// component (cmake -DLIBRMCS_ENABLE_IGH=ON), so the default SDK build keeps
// zero new dependencies. It can coexist with LIBRMCS_ENABLE_SOEM: different
// namespace, different create_transport(), the caller (examples) picks one.
//
// Design notes (identical in spirit to soem.cpp, only the master calls differ):
//  * One dedicated cycle thread drives the master in free-run mode: build
//    outputs, send process data, wait for the frame to return, feed the ARQ
//    endpoint. Poll pacing is pure busy-spin for the lowest latency (pair it
//    with options.thread_setup pinning the thread to an isolated core).
//  * The exchange can run over several independent process-data channels
//    (RMCS_ECAT_PIPELINE, default 1 = stop-and-wait). One channel == at most
//    one frame in flight, because IgH refuses to queue a datagram that is
//    already queued (it only bumps skip_count), so depth has to come from
//    several domains, and each domain costs the slave one FMMU per sync
//    manager.
//
//    WHETHER DEPTH HELPS DEPENDS ON THE TRAFFIC (measured, ecat/DESIGN.md 4.5).
//    Request/response, where the reply's timing is correlated with the frame
//    that carried the request, gets no phase saving and only pays the cost:
//
//        depth 1: 38.0 kHz, rtt p50 133.4 us   <- best here
//        depth 2: 77.8 kHz, rtt p50 139.4 us
//        depth 4: 84.3 kHz, rtt p50 155.5 us   (wire saturated at 99.8 Mbit/s)
//
//    Continuous uplink flood, where CAN frames arrive at a phase unrelated to
//    the frames, is the opposite -- and it is the shape of the real workload:
//
//        depth 1: rtt p50 159.7, p90 177.6, max 0.41-0.44 ms
//        depth 2: rtt p50 150.9, p90 161.9, max 2.5-9.7 ms   <- p50 -8.6 us
//
//    THE DEFAULT STAYS 1 BECAUSE OF THAT TAIL, not because depth is useless:
//    9.7 ms is ten missed cycles of a 1 kHz control loop. The tail has the
//    shape of go-back-N replay, but PdStreamEndpoint has no counter on that
//    path, so it is unproven -- instrument it before raising the default.
//
//    Depth is additionally capped by the ARQ window (kPdWindow == 2): beyond
//    two frames in flight the endpoint has no credit left to stage new chunks
//    and the extra channels can only repaint the newest one.
//  * Stream reliability comes from the go-back-N ARQ in
//    librmcs::ecat::PdStreamEndpoint (shared with the firmware, roles are
//    symmetric), exactly as in the SOEM backend.
//  * transmit() only copies the frame into a byte ring; the cycle thread
//    drains it one PDO payload per acknowledged chunk. When the ring is full the
//    caller spins briefly -- end-to-end backpressure, frames are never dropped.
//  * Cycles that delivered payload grant the application a short response
//    window before the next poll is built, so an immediate answer rides this
//    cycle instead of the next one.
//
// Distributed clock: none. The bridge runs SM-synchron and never programs
// SYNC0, so DC sync would be pure per-frame overhead: the application-time/
// reference-clock/slave-clock trio costs 3 ioctls per cycle and appends DC
// register datagrams to every frame, right on the 100Mbit serialization
// bottleneck. The master would still auto-elect this DC-capable slave as the
// reference clock (and then refuse OP until the trio is fed every cycle --
// verified live, see DESIGN.md), so the constructor explicitly selects NO
// reference clock via ecrt_master_select_reference_clock(master, nullptr).

#if defined(LIBRMCS_ENABLE_IGH)

# include <algorithm>
# include <array>
# include <atomic>
# include <chrono>
# include <cstddef>
# include <cstdint>
# include <cstdlib>
# include <cstring>
# include <format>
# include <functional>
# include <memory>
# include <mutex>
# include <span>
# include <stdexcept>
# include <string>
# include <string_view>
# include <thread>
# include <utility>
# include <vector>

# include <ecrt.h>

# include <librmcs/ecat/hybrid_pd.hpp>
# include <librmcs/ecat/native_can.hpp>
# include <librmcs/ecat/pd_stream.hpp>

# include "core/src/protocol/constant.hpp"
# include "core/src/utility/assert.hpp"
# include "host/src/logging/logging.hpp"
# include "host/src/transport/transport.hpp"
# include "host/src/utility/final_action.hpp"

namespace librmcs::host::transport::igh {

namespace {

namespace ecat = librmcs::ecat;

constexpr std::size_t kStreamChunkSize = ecat::kPdChunkSize;

// Fixed identity + PDO layout of the rmcs_board slave, read from the live bus
// with `ethercat slaves -v` / `ethercat pdos -p 0`. The slave reports
// "PDO Assign: no" / "PDO Configuration: no", so ecrt_slave_config_pdos() only
// describes the existing mapping, it does not change it. SM2 = RxPDO 0x1600
// (master->slave, 12 x 32-bit at 0x7010:01..0C). SM3 = TxPDO 0x1a00
// (slave->master, 12 x 32-bit at 0x6000:01..0C). Both are 48 bytes == one
// kPdChunkSize chunk. If the slave firmware/board changes, re-read the bus.
constexpr uint32_t kVendorId = 0x00001A81;
constexpr uint32_t kProductCode = 0x00000001;
constexpr uint16_t kRxPdoIndex = 0x1600;
constexpr uint16_t kTxPdoIndex = 0x1a00;
constexpr uint16_t kOutputEntryIndex = 0x7010; // master -> slave subindex base
constexpr uint16_t kInputEntryIndex = 0x6000;  // slave -> master subindex base
constexpr unsigned kPdoEntryCount = 12;

constexpr uint16_t kHybridOutputFixedIndex = 0x7000;
constexpr uint16_t kHybridOutputStreamIndex = 0x7010;
constexpr uint16_t kHybridInputFixedIndex = 0x6000;
constexpr uint16_t kHybridInputStreamIndex = 0x6010;
constexpr unsigned kHybridFixedEntryCount = ecat::kHybridMailboxRegionSize / sizeof(uint32_t);
constexpr unsigned kHybridStreamEntryCount = ecat::kHybridStreamChunkSize / sizeof(uint32_t);
constexpr unsigned kHybridPdoEntryCount = kHybridFixedEntryCount + kHybridStreamEntryCount;
static_assert(kHybridFixedEntryCount == 84);
static_assert(kHybridPdoEntryCount == 88);

// AL state word bit for OPERATIONAL in ec_master_state_t::al_states.
constexpr uint8_t kAlStateOp = 1u << 3;

std::string_view al_state_name(unsigned int al_state) noexcept {
    switch (al_state & 0x0F) {
    case 0x01: return "INIT";
    case 0x02: return "PREOP";
    case 0x04: return "SAFEOP";
    case 0x08: return "OP";
    default: return "MIXED";
    }
}

// Byte ring between transmit() callers and the cycle thread. A mutex is fine
// here: the critical sections are single memcpys and the cycle thread only
// takes it once per poll. (Identical to the SOEM backend's LockedByteRing.)
class LockedByteRing {
public:
    static constexpr std::size_t kSize = std::size_t{64} * 1024; // power of two

    bool try_push(std::span<const std::byte> data) noexcept {
        const std::scoped_lock guard{mutex_};
        if (kSize - (in_ - out_) < data.size())
            return false;
        const std::size_t offset = in_ & (kSize - 1);
        const std::size_t slice = std::min(data.size(), kSize - offset);
        std::memcpy(buffer_ + offset, data.data(), slice);
        std::memcpy(buffer_, data.data() + slice, data.size() - slice);
        in_ += data.size();
        return true;
    }

    std::size_t pop(std::span<std::byte> destination) noexcept {
        const std::scoped_lock guard{mutex_};
        const std::size_t count = std::min<std::size_t>(in_ - out_, destination.size());
        if (count == 0)
            return 0;
        const std::size_t offset = out_ & (kSize - 1);
        const std::size_t slice = std::min(count, kSize - offset);
        std::memcpy(destination.data(), buffer_ + offset, slice);
        std::memcpy(destination.data() + slice, buffer_, count - slice);
        out_ += count;
        return count;
    }

    bool empty() noexcept {
        const std::scoped_lock guard{mutex_};
        return in_ == out_;
    }

    void clear() noexcept {
        const std::scoped_lock guard{mutex_};
        out_ = in_;
    }

private:
    std::mutex mutex_;
    std::size_t in_ = 0;
    std::size_t out_ = 0;
    std::byte buffer_[kSize];
};

// Receive-side sink handed to PdStreamEndpoint::on_peer_chunk(): delivers the
// payload straight to the user callback, so the ack is never withheld.
// delivered flags cycles that handed fresh payload to the application -- the
// cycle loop grants those a short response window (see cycle_loop()).
struct CallbackSink {
    std::function<void(std::span<const std::byte>)>& callback;
    bool& delivered;

    bool try_push(std::span<const std::byte> data) const {
        delivered = true;
        if (callback)
            callback(data);
        return true;
    }
};

class IghBuffer final : public TransportBuffer {
public:
    explicit IghBuffer(bool hybrid) noexcept
        : hybrid_(hybrid) {}

    BufferSpanType data() const noexcept override { return BufferSpanType{storage_}; }

    bool try_stage_cyclic_can(
        data::DataId field_id, const data::CanDataView& view) noexcept override {
        if (!hybrid_)
            return false;
        has_cyclic_data_ = true;
        if (cyclic_invalid_ || view.is_extended_can_id || view.is_remote_transmission
            || view.can_id > 0x7FFU || view.can_data.size() > ecat::kNativeMaxDataSize) {
            invalidate_cyclic_batch();
            return false;
        }

        const int bus = can_bus(field_id);
        if (bus < 0 || cyclic_counts_[static_cast<std::size_t>(bus)] >= ecat::kHybridSlotsPerBus) {
            invalidate_cyclic_batch();
            return false;
        }

        const std::size_t bus_index = static_cast<std::size_t>(bus);
        const std::size_t slot = cyclic_counts_[bus_index]++;
        std::byte* mailbox = cyclic_image_.data() + ecat::hybrid_mailbox_offset(bus_index, slot);
        std::memset(mailbox, 0, ecat::kNativeMailboxSize);
        mailbox[ecat::kNativeMetaOffset] = static_cast<std::byte>(
            ecat::native_meta(view.is_fdcan, static_cast<std::uint8_t>(view.can_data.size())));
        mailbox[ecat::kNativeIdOffset] = static_cast<std::byte>(view.can_id);
        mailbox[ecat::kNativeIdOffset + 1] = static_cast<std::byte>(view.can_id >> 8);
        if (!view.can_data.empty()) {
            std::memcpy(
                mailbox + ecat::kNativeDataOffset, view.can_data.data(), view.can_data.size());
        }
        return true;
    }

    bool begin_cyclic_can_batch() noexcept override {
        if (!hybrid_)
            return false;
        has_cyclic_data_ = true;
        return true;
    }

    void reject_cyclic_can_batch() noexcept override {
        if (hybrid_) {
            has_cyclic_data_ = true;
            invalidate_cyclic_batch();
        }
    }

    bool has_cyclic_data() const noexcept override { return has_cyclic_data_; }

    void reset() noexcept {
        cyclic_counts_.fill(0);
        has_cyclic_data_ = false;
        cyclic_invalid_ = false;
    }

    const std::array<std::uint8_t, ecat::kNativeBusCount>& cyclic_counts() const noexcept {
        return cyclic_counts_;
    }

    const std::array<std::byte, ecat::kHybridMailboxRegionSize>& cyclic_image() const noexcept {
        return cyclic_image_;
    }

private:
    void invalidate_cyclic_batch() noexcept {
        cyclic_counts_.fill(0);
        cyclic_invalid_ = true;
    }

    static int can_bus(data::DataId field_id) noexcept {
        switch (field_id) {
        case data::DataId::kCan0: return 0;
        case data::DataId::kCan1: return 1;
        case data::DataId::kCan2: return 2;
        case data::DataId::kCan3: return 3;
        default: return -1;
        }
    }

    const bool hybrid_;
    bool has_cyclic_data_ = false;
    bool cyclic_invalid_ = false;
    std::array<std::uint8_t, ecat::kNativeBusCount> cyclic_counts_{};
    std::array<std::byte, ecat::kHybridMailboxRegionSize> cyclic_image_{};
    mutable std::byte storage_[core::protocol::kProtocolBufferSize];
};

// Single-consumer triple buffer for complete cyclic snapshots. The producer
// owns one bank, the cycle thread owns one bank, and the atomic middle bank is
// the hand-off point. Publishing again before the cycle thread samples the
// middle bank replaces the older snapshot without ever making a partially
// copied image visible.
struct CyclicBatch {
    std::array<std::byte, ecat::kHybridMailboxRegionSize> image{};
    std::array<std::uint8_t, ecat::kNativeBusCount> counts{};
};

constexpr std::uint32_t kCyclicBankIndexMask = 0x3U;
constexpr std::uint32_t kCyclicBankDirty = 0x4U;
constexpr std::size_t kCyclicBankCount = 3;
static_assert(kCyclicBankCount <= kCyclicBankIndexMask + 1U);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// One pipelined process-data channel: its own domain, hence its own datagrams,
// its own process-data memory and its own FMMU pair on the slave (IgH allocates
// a new FMMU per domain+sync-manager, so N channels cost 2N of the slave's
// FMMUs -- check `ethercat slaves -v` before raising the depth).
struct PdChannel {
    ec_domain_t* domain = nullptr;
    uint8_t* process_data = nullptr;
    std::byte* outputs = nullptr;
    std::byte* inputs = nullptr;
    std::byte* stream_outputs = nullptr;
    std::byte* stream_inputs = nullptr;
};

// Upper bound on frames in flight. Depth buys frame RATE; whether that also
// buys latency depends on the traffic -- see the file header for both cases.
//
// 4 is also the hardware ceiling, not an arbitrary limit: the slave's ESC
// reports 8 FMMUs (register 0x0004, read back on the board) and every channel
// consumes one per sync manager, so depth 4 uses all 8.
constexpr unsigned kMaxPipelineDepth = 4;

class Igh final : public Transport {
public:
    Igh(std::string_view interface_name, const ConnectionOptions& options)
        : logger_(logging::get_logger()) {
        // The IgH target device is chosen at the ethercat.conf / master-index
        // level (ecrt_request_master), not by binding a socket to an interface
        // like SOEM does. interface_name is therefore advisory here: it is
        // logged for parity with the SOEM API but does not select the NIC.
        const std::string ifname{interface_name};

        const char* mode_env = std::getenv("RMCS_ECAT_MODE");
        const std::string_view mode = mode_env ? mode_env : "stream";
        if (mode == "hybrid") {
            hybrid_mode_ = true;
        } else if (mode != "stream") {
            throw std::runtime_error{
                "unknown RMCS_ECAT_MODE (expected stream or hybrid)"};
        }

        // Frames in flight. 1 is stop-and-wait and stays the default because
        // depth 2 blows the worst-case tail out to milliseconds under load,
        // even though it wins the body of the distribution (file header).
        if (const char* depth_env = std::getenv("RMCS_ECAT_PIPELINE")) {
            const long value = std::strtol(depth_env, nullptr, 10);
            if (value < 1 || value > static_cast<long>(kMaxPipelineDepth))
                throw std::runtime_error{std::format(
                    "RMCS_ECAT_PIPELINE={} out of range (expected 1..{})", depth_env,
                    kMaxPipelineDepth)};
            pipeline_depth_ = static_cast<unsigned>(value);
        }

        master_ = ecrt_request_master(0);
        if (!master_)
            throw std::runtime_error{
                "ecrt_request_master(0) failed (is the ethercat service running? "
                "run: sudo /usr/local/sbin/ethercatctl start)"};
        utility::FinalAction release_on_failure{[this]() noexcept {
            ecrt_release_master(master_);
            master_ = nullptr;
        }};

        for (unsigned i = 0; i < pipeline_depth_; ++i) {
            channels_[i].domain = ecrt_master_create_domain(master_);
            if (!channels_[i].domain)
                throw std::runtime_error{
                    std::format("ecrt_master_create_domain failed for channel {}", i)};
        }

        slave_config_ = ecrt_master_slave_config(master_, 0, 0, kVendorId, kProductCode);
        if (!slave_config_)
            throw std::runtime_error{std::format(
                "ecrt_master_slave_config failed (expected vendor 0x{:08X} product 0x{:08X} "
                "at ring position 0)",
                kVendorId, kProductCode)};

        // Describe either the stock 48-byte stream mapping or the explicit
        // 352-byte hybrid mapping. PDO assignment/configuration stays fixed in
        // the slave; ecrt uses this description to resolve domain offsets.
        std::array<ec_pdo_entry_info_t, kHybridPdoEntryCount> out_entries{};
        std::array<ec_pdo_entry_info_t, kHybridPdoEntryCount> in_entries{};
        unsigned pdo_entry_count = kPdoEntryCount;
        if (hybrid_mode_) {
            pdo_entry_count = kHybridPdoEntryCount;
            for (unsigned i = 0; i < kHybridFixedEntryCount; ++i) {
                out_entries[i] = {
                    kHybridOutputFixedIndex, static_cast<uint8_t>(i + 1), 32};
                in_entries[i] = {
                    kHybridInputFixedIndex, static_cast<uint8_t>(i + 1), 32};
            }
            for (unsigned i = 0; i < kHybridStreamEntryCount; ++i) {
                out_entries[kHybridFixedEntryCount + i] = {
                    kHybridOutputStreamIndex, static_cast<uint8_t>(i + 1), 32};
                in_entries[kHybridFixedEntryCount + i] = {
                    kHybridInputStreamIndex, static_cast<uint8_t>(i + 1), 32};
            }
        } else {
            for (unsigned i = 0; i < kPdoEntryCount; ++i) {
                out_entries[i] = {kOutputEntryIndex, static_cast<uint8_t>(i + 1), 32};
                in_entries[i] = {kInputEntryIndex, static_cast<uint8_t>(i + 1), 32};
            }
        }
        ec_pdo_info_t pdo_out[] = {
            {kRxPdoIndex, pdo_entry_count, out_entries.data()}
        };
        ec_pdo_info_t pdo_in[] = {
            {kTxPdoIndex, pdo_entry_count, in_entries.data()}
        };
        ec_sync_info_t syncs[] = {
            {   0,  EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
            {   1,   EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
            {   2,  EC_DIR_OUTPUT, 1, pdo_out,  EC_WD_ENABLE},
            {   3,   EC_DIR_INPUT, 1,  pdo_in, EC_WD_DISABLE},
            {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
        };
        if (ecrt_slave_config_pdos(slave_config_, EC_END, syncs))
            throw std::runtime_error{"ecrt_slave_config_pdos failed"};

        // No DC reference clock (see the file header): without this the master
        // auto-elects the slave as reference clock and the FSM never reaches
        // OP unless the DC trio is fed every cycle.
        if (ecrt_master_select_reference_clock(master_, nullptr))
            throw std::runtime_error{"ecrt_master_select_reference_clock(none) failed"};

        const uint16_t output_base_index =
            hybrid_mode_ ? kHybridOutputFixedIndex : kOutputEntryIndex;
        const uint16_t input_base_index =
            hybrid_mode_ ? kHybridInputFixedIndex : kInputEntryIndex;
        // Registering the same PDO entry in a second domain is what allocates
        // the second FMMU pair: ec_slave_config_prepare_fmmu() only reuses an
        // FMMU when domain AND sync manager match. Offsets are per-domain.
        std::array<std::size_t, kMaxPipelineDepth> off_out{};
        std::array<std::size_t, kMaxPipelineDepth> off_in{};
        for (unsigned i = 0; i < pipeline_depth_; ++i) {
            const int out = ecrt_slave_config_reg_pdo_entry(
                slave_config_, output_base_index, 1, channels_[i].domain, nullptr);
            const int in = ecrt_slave_config_reg_pdo_entry(
                slave_config_, input_base_index, 1, channels_[i].domain, nullptr);
            if (out < 0 || in < 0)
                throw std::runtime_error{std::format(
                    "ecrt_slave_config_reg_pdo_entry failed for channel {} (out={} in={}); "
                    "pipeline depth {} needs {} FMMUs on the slave",
                    i, out, in, pipeline_depth_, 2 * pipeline_depth_)};
            off_out[i] = static_cast<std::size_t>(out);
            off_in[i] = static_cast<std::size_t>(in);
        }

        if (ecrt_master_activate(master_))
            throw std::runtime_error{"ecrt_master_activate failed"};
        // After activate(), deactivate() must run before release() on cleanup.
        utility::FinalAction deactivate_on_failure{
            [this]() noexcept { ecrt_master_deactivate(master_); }};

        for (unsigned i = 0; i < pipeline_depth_; ++i) {
            PdChannel& channel = channels_[i];
            channel.process_data = ecrt_domain_data(channel.domain);
            if (!channel.process_data)
                throw std::runtime_error{std::format(
                    "ecrt_domain_data returned null after activate for channel {}", i)};
            channel.outputs = reinterpret_cast<std::byte*>(channel.process_data + off_out[i]);
            channel.inputs = reinterpret_cast<std::byte*>(channel.process_data + off_in[i]);
            channel.stream_outputs =
                channel.outputs + (hybrid_mode_ ? ecat::kHybridStreamRegionOffset : 0);
            channel.stream_inputs =
                channel.inputs + (hybrid_mode_ ? ecat::kHybridStreamRegionOffset : 0);
        }

        // Both sides restart from seq/ack 0 on every OP (re)entry: the slave
        // resets in APPL_StartOutputHandler(), we reset here, before driving
        // the master FSM up to OP.
        reset_endpoint();
        for (unsigned i = 0; i < pipeline_depth_; ++i)
            std::memset(channels_[i].outputs, 0, process_data_size());

        drive_to_operational();

        expected_wc_ = EC_WC_COMPLETE;
        logger_.info(
            R"(EtherCAT (IgH) {} link up (interface hint "{}"): {}B PDO, {}B stream chunk, )"
            R"(pipeline depth {}, vendor 0x{:08X} product 0x{:08X})",
            hybrid_mode_ ? "hybrid" : "stream", ifname, process_data_size(),
            stream_chunk_size(), pipeline_depth_, kVendorId, kProductCode);

        if (options.thread_setup) {
            std::atomic<bool> thread_setup_done{false};
            cycle_thread_ = std::thread{[this, &options, &thread_setup_done]() {
                options.thread_setup(options);
                thread_setup_done.store(true, std::memory_order_release);
                thread_setup_done.notify_one();
                cycle_loop();
            }};
            thread_setup_done.wait(false, std::memory_order_acquire);
        } else {
            cycle_thread_ = std::thread{[this]() { cycle_loop(); }};
        }

        deactivate_on_failure.disable();
        release_on_failure.disable();
    }

    Igh(const Igh&) = delete;
    Igh& operator=(const Igh&) = delete;
    Igh(Igh&&) = delete;
    Igh& operator=(Igh&&) = delete;

    ~Igh() override {
        stop_.store(true, std::memory_order_relaxed);
        if (cycle_thread_.joinable())
            cycle_thread_.join();
        logger_.info(
            "EtherCAT (IgH) link closing: {} cycles total, {} working-counter errors",
            total_cycles_, total_wc_errors_);
        ecrt_master_deactivate(master_);
        ecrt_release_master(master_);
    }

    std::unique_ptr<TransportBuffer> acquire_transmit_buffer() noexcept override {
        {
            const std::scoped_lock guard{buffer_pool_mutex_};
            if (!buffer_pool_.empty()) {
                auto buffer = std::move(buffer_pool_.back());
                buffer_pool_.pop_back();
                static_cast<IghBuffer&>(*buffer).reset();
                return buffer;
            }
        }
        return std::make_unique<IghBuffer>(hybrid_mode_);
    }

    void transmit(std::unique_ptr<TransportBuffer> buffer, size_t payload_size) override {
        core::utility::assert_always(buffer != nullptr);
        core::utility::assert_always(payload_size <= core::protocol::kProtocolBufferSize);
        auto& igh_buffer = static_cast<IghBuffer&>(*buffer);
        if (igh_buffer.has_cyclic_data())
            commit_cyclic_can(igh_buffer);

        const std::span<const std::byte> payload{buffer->data().data(), payload_size};
        // Spin until the ring accepts the whole frame: the stream must stay
        // lossless, and sustained fullness means the link (44B per
        // acknowledged chunk) is saturated -- backpressure is correct then.
        if (!payload.empty()) {
            while (!transmit_ring_.try_push(payload)) {
                if (stop_.load(std::memory_order_relaxed))
                    return;
                std::this_thread::yield();
            }
        }
        recycle_buffer(std::move(buffer));
    }

    void release_transmit_buffer(std::unique_ptr<TransportBuffer> buffer) override {
        recycle_buffer(std::move(buffer));
    }

    void receive(std::function<void(std::span<const std::byte>)> callback) override {
        core::utility::assert_always(static_cast<bool>(callback));
        core::utility::assert_always(!receive_callback_registered_.load(std::memory_order_acquire));
        {
            const std::scoped_lock guard{receive_callback_mutex_};
            receive_callback_ = std::move(callback);
        }
        receive_callback_registered_.store(true, std::memory_order_release);
    }

    void receive_cyclic_can(
        std::function<void(data::DataId, const data::CanDataView&)> callback) override {
        core::utility::assert_always(static_cast<bool>(callback));
        core::utility::assert_always(
            !cyclic_receive_callback_registered_.load(std::memory_order_acquire));
        cyclic_receive_callback_ = std::move(callback);
        cyclic_receive_callback_registered_.store(true, std::memory_order_release);
    }

    void on_link_restart(std::function<void()> callback) override {
        core::utility::assert_always(static_cast<bool>(callback));
        core::utility::assert_always(
            !link_restart_callback_registered_.load(std::memory_order_acquire));
        link_restart_callback_ = std::move(callback);
        link_restart_callback_registered_.store(true, std::memory_order_release);
    }

private:
    std::size_t process_data_size() const noexcept {
        return hybrid_mode_ ? ecat::kHybridPdSize : ecat::kPdChunkSize;
    }

    std::size_t stream_chunk_size() const noexcept {
        return hybrid_mode_ ? ecat::kHybridPdChunkSize : ecat::kPdChunkSize;
    }

    void reset_endpoint() noexcept {
        if (hybrid_mode_)
            hybrid_endpoint_.reset();
        else
            endpoint_.reset();
    }

    // The ARQ endpoint is shared by all channels and stays correct because the
    // loop keeps on_peer_chunk() and build_own_chunk() paired one-to-one per
    // frame: the pd_stream window credit is "one new chunk per received frame",
    // and each channel's image is rebuilt immediately before its own send, so
    // wire order equals build order.
    void build_stream_output(const PdChannel& channel) noexcept {
        if (hybrid_mode_)
            hybrid_endpoint_.build_own_chunk(channel.stream_outputs, transmit_ring_);
        else
            endpoint_.build_own_chunk(channel.stream_outputs, transmit_ring_);
    }

    void consume_stream_input(const PdChannel& channel, CallbackSink& sink) noexcept {
        if (hybrid_mode_)
            hybrid_endpoint_.on_peer_chunk(channel.stream_inputs, sink);
        else
            endpoint_.on_peer_chunk(channel.stream_inputs, sink);
    }

    void commit_cyclic_can(const IghBuffer& buffer) noexcept {
        if (!hybrid_mode_)
            return;

        // PacketBuilder is normally a single control-loop producer. Keep a
        // producer-only mutex so accidental concurrent builders remain safe;
        // the real-time cycle thread never takes this lock.
        const std::scoped_lock guard{cyclic_producer_mutex_};
        CyclicBatch& staged = cyclic_banks_[cyclic_producer_bank_];
        staged.image = buffer.cyclic_image();
        staged.counts = buffer.cyclic_counts();

        const std::uint32_t released = cyclic_middle_state_.exchange(
            cyclic_producer_bank_ | kCyclicBankDirty, std::memory_order_acq_rel);
        cyclic_producer_bank_ = released & kCyclicBankIndexMask;
    }

    void apply_pending_cyclic_output() noexcept {
        if ((cyclic_middle_state_.load(std::memory_order_acquire) & kCyclicBankDirty) == 0)
            return;

        // Swapping is the only cycle-thread operation on the hand-off state.
        // If the producer publishes between the load and exchange, the
        // exchange simply takes that newer bank, preserving latest-wins.
        const std::uint32_t published = cyclic_middle_state_.exchange(
            cyclic_consumer_bank_, std::memory_order_acq_rel);
        cyclic_consumer_bank_ = published & kCyclicBankIndexMask;
        const CyclicBatch& pending = cyclic_banks_[cyclic_consumer_bank_];

        // A batch is a complete latest-wins snapshot. Slots omitted by the
        // newest batch become invalid (seq 0), so an older partially sampled
        // batch cannot leak through later. Per-slot counters live separately
        // from the image so reusing a slot after an idle gap still gets a fresh
        // nonzero generation.
        for (std::size_t mailbox_index = 0; mailbox_index < ecat::kHybridMailboxCount;
             ++mailbox_index) {
            cyclic_output_image_[mailbox_index * ecat::kNativeMailboxSize] = std::byte{0};
        }

        for (std::size_t bus = 0; bus < ecat::kNativeBusCount; ++bus) {
            for (std::size_t slot = 0; slot < pending.counts[bus]; ++slot) {
                const std::size_t offset = ecat::hybrid_mailbox_offset(bus, slot);
                const std::size_t mailbox_index = ecat::hybrid_mailbox_index(bus, slot);
                std::byte* destination = cyclic_output_image_.data() + offset;
                const std::byte* source = pending.image.data() + offset;
                std::memcpy(
                    destination + ecat::kNativeMetaOffset,
                    source + ecat::kNativeMetaOffset,
                    ecat::kNativeMailboxSize - ecat::kNativeMetaOffset);

                std::uint8_t seq = cyclic_output_seq_[mailbox_index];
                seq = seq == 255 ? 1 : static_cast<std::uint8_t>(seq + 1);
                cyclic_output_seq_[mailbox_index] = seq;
                destination[ecat::kNativeSeqOffset] = static_cast<std::byte>(seq);
            }
        }
    }

    void publish_cyclic_outputs(const PdChannel& channel) noexcept {
        if (!hybrid_mode_)
            return;
        // Latest-wins: a channel that finds no newly published snapshot simply
        // re-paints the live image, so the fixed region is identical on every
        // frame of the pipeline regardless of which one consumed the batch.
        apply_pending_cyclic_output();
        std::memcpy(channel.outputs, cyclic_output_image_.data(), cyclic_output_image_.size());
    }

    void clear_cyclic_outputs(bool reset_sequences) noexcept {
        if (!hybrid_mode_)
            return;

        // Recovery is off the healthy hot path. Serialize with producers, take
        // and discard the currently published bank, then reset the live image.
        const std::scoped_lock guard{cyclic_producer_mutex_};
        const std::uint32_t published = cyclic_middle_state_.exchange(
            cyclic_consumer_bank_, std::memory_order_acq_rel);
        cyclic_consumer_bank_ = published & kCyclicBankIndexMask;
        cyclic_output_image_.fill(std::byte{0});
        if (reset_sequences)
            cyclic_output_seq_.fill(0);
    }

    void notify_link_restart() {
        if (link_restart_callback_registered_.load(std::memory_order_acquire))
            link_restart_callback_();
    }

    static data::DataId can_data_id(std::size_t bus) noexcept {
        constexpr std::array ids = {
            data::DataId::kCan0,
            data::DataId::kCan1,
            data::DataId::kCan2,
            data::DataId::kCan3,
        };
        return ids[bus];
    }

    void deliver_cyclic_inputs(const PdChannel& channel) {
        if (!hybrid_mode_
            || !cyclic_receive_callback_registered_.load(std::memory_order_acquire)) {
            return;
        }

        for (std::size_t mailbox_index = 0; mailbox_index < ecat::kHybridMailboxCount;
             ++mailbox_index) {
            const std::byte* mailbox =
                channel.inputs + mailbox_index * ecat::kNativeMailboxSize;
            const std::uint8_t seq =
                static_cast<std::uint8_t>(mailbox[ecat::kNativeSeqOffset]);
            if (seq == 0 || seq == cyclic_input_seq_[mailbox_index])
                continue;
            cyclic_input_seq_[mailbox_index] = seq;

            const std::uint8_t meta =
                static_cast<std::uint8_t>(mailbox[ecat::kNativeMetaOffset]);
            const std::uint8_t length = ecat::native_meta_length(meta);
            if (length > ecat::kNativeMaxDataSize)
                continue;

            const std::uint32_t can_id =
                static_cast<std::uint8_t>(mailbox[ecat::kNativeIdOffset])
                | static_cast<std::uint32_t>(
                      static_cast<std::uint8_t>(mailbox[ecat::kNativeIdOffset + 1]))
                      << 8;
            const data::CanDataView view{
                .can_id = can_id,
                .can_data = {mailbox + ecat::kNativeDataOffset, length},
                .is_fdcan = ecat::native_meta_is_fdcan(meta),
                .is_extended_can_id = false,
                .is_remote_transmission = false,
            };
            cyclic_receive_callback_(
                can_data_id(mailbox_index / ecat::kHybridSlotsPerBus), view);
        }
    }

    void recycle_buffer(std::unique_ptr<TransportBuffer> buffer) {
        if (!buffer)
            return;
        const std::scoped_lock guard{buffer_pool_mutex_};
        if (buffer_pool_.size() < kBufferPoolLimit)
            buffer_pool_.push_back(std::move(buffer));
    }

    // Build one channel's output image and put THAT channel on the wire.
    //
    // Queueing several domains before a single ecrt_master_send() would let IgH
    // pack them into one frame (two 48B domains fit far below the MTU), which is
    // the exact opposite of a pipeline -- so each channel gets its own send.
    // No DC trio here: the constructor selects no reference clock, so the frame
    // carries only the process data datagrams (see the file header).
    void send_channel(unsigned index) noexcept {
        const PdChannel& channel = channels_[index];
        // Remember whether THIS frame is a zero-image probe. At depth > 1 the
        // frames already in flight when the discontinuity was detected still
        // carry pre-error data, and only a frame that actually went out zeroed
        // is allowed to end the discontinuity (see cycle_loop()).
        channel_zero_probe_[index] = process_data_discontinuity_;
        if (process_data_discontinuity_) {
            // Do not present any old generation to a slave that may have reset
            // during a short SAFEOP round trip. The first complete exchange is a
            // zero-image probe; normal publication resumes only after its peer
            // state has been classified in cycle_loop().
            if (hybrid_mode_)
                std::memset(channel.outputs, 0, ecat::kHybridMailboxRegionSize);
            std::memset(channel.stream_outputs, 0, stream_chunk_size());
        } else {
            publish_cyclic_outputs(channel);
            build_stream_output(channel);
        }
        ecrt_domain_queue(channel.domain);
        ecrt_master_send(master_);
    }

    // Poll until this channel's frame returns. Returns the domain
    // working-counter state observed then (or after the response deadline
    // elapsed).
    //
    // ecrt_master_receive() is non-blocking and global: it drains whatever
    // arrived, including the other channels' frames, whose datagrams are then
    // marked received and dispatched into their own domain memory. Only the
    // requested domain is evaluated here; the others are evaluated when their
    // turn comes. A domain whose datagram has been re-queued but not yet
    // received sums to working counter 0, i.e. EC_WC_ZERO -- that is what makes
    // this poll terminate on the right frame.
    ec_domain_state_t wait_for_channel(unsigned index) noexcept {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + kResponseTimeout;
        ec_domain_state_t ds{};
        for (;;) {
            ecrt_master_receive(master_);
            ecrt_domain_process(channels_[index].domain);
            ecrt_domain_state(channels_[index].domain, &ds);
            if (ds.wc_state == EC_WC_COMPLETE)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                break; // dropped cycle; caller counts it as a wc error
        }
        return ds;
    }

    // Stop-and-wait exchange of whatever the caller has placed in the channel's
    // output image. Used by the warm-up, which drives a zero image: the master
    // FSM walks the AL states over many cycles and gains nothing from a
    // pipeline, so priming the pipeline is left to cycle_loop().
    ec_domain_state_t exchange_once(unsigned index) noexcept {
        ecrt_domain_queue(channels_[index].domain);
        ecrt_master_send(master_);
        return wait_for_channel(index);
    }

    // Warm-up: drive the master's internal FSM until the slave reports OP.
    // ecrt has no explicit ec_writestate(OPERATIONAL) like SOEM: the FSM walks
    // PREOP -> SAFEOP -> OP on its own as long as the application keeps cycling
    // process data (with the DC trio). Throws on timeout.
    void drive_to_operational() {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + kOperationalTimeout;
        for (;;) {
            std::memset(channels_[0].outputs, 0, process_data_size());
            exchange_once(0);

            ec_master_state_t ms{};
            ecrt_master_state(master_, &ms);
            if (ms.link_up && ms.slaves_responding && (ms.al_states & kAlStateOp))
                return;

            if (std::chrono::steady_clock::now() >= deadline) {
                ec_master_state_t last{};
                ec_slave_config_state_t slave_state{};
                ec_domain_state_t domain_state{};
                ecrt_master_state(master_, &last);
                ecrt_slave_config_state(slave_config_, &slave_state);
                ecrt_domain_state(channels_[0].domain, &domain_state);
                throw std::runtime_error{std::format(
                    "EtherCAT slave did not reach OPERATIONAL within {} s "
                    "(link_up={} slaves_responding={} master_al_states=0x{:02X} "
                    "slave_online={} slave_operational={} slave_al_state=0x{:X}/{} "
                    "domain_wc_state={} domain_wc={}; verify the live SII/PDO mapping is "
                    "{} bytes / {} entries per direction and check `ethercat slaves -v` or dmesg "
                    "for the AL status code. Pipeline depth is {}, which needs {} FMMUs on the "
                    "slave -- if raising it caused this, the slave ran out of FMMUs and the "
                    "config FSM stalls before SAFEOP)",
                    std::chrono::duration_cast<std::chrono::seconds>(kOperationalTimeout).count(),
                    static_cast<unsigned>(last.link_up),
                    static_cast<unsigned>(last.slaves_responding),
                    static_cast<unsigned>(last.al_states),
                    static_cast<unsigned>(slave_state.online),
                    static_cast<unsigned>(slave_state.operational),
                    static_cast<unsigned>(slave_state.al_state),
                    al_state_name(slave_state.al_state),
                    static_cast<unsigned>(domain_state.wc_state),
                    static_cast<unsigned>(domain_state.working_counter), process_data_size(),
                    hybrid_mode_ ? kHybridPdoEntryCount : kPdoEntryCount, pipeline_depth_,
                    2 * pipeline_depth_)};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    void cycle_loop() {
        using Clock = std::chrono::steady_clock;

        // Diagnostic-only pacing. Free-running (0) is the production behaviour;
        // setting RMCS_ECAT_CYCLE_US spaces the exchanges out so that the round
        // trip can be measured against several cycle periods. Fitting
        // RTT = N * T_cycle + C over those points separates the part that scales
        // with the poll rate (N cycles of pipeline and phase) from the fixed
        // part (CAN wire time plus board and host processing) -- a single
        // measurement cannot, because it has two unknowns.
        //
        // Busy-wait rather than sleep: a sleep would hand the core back and
        // reintroduce wakeup jitter and C-state exit into the very number being
        // measured.
        std::chrono::nanoseconds pace{0};
        if (const char* pace_env = std::getenv("RMCS_ECAT_CYCLE_US")) {
            const long value = std::strtol(pace_env, nullptr, 10);
            if (value > 0)
                pace = std::chrono::microseconds{value};
        }
        Clock::time_point next_cycle = Clock::now();

        uint32_t wc_error_streak = 0;
        Clock::time_point window_start = Clock::now();
        uint64_t window_cycles = 0;
        uint64_t window_wc_errors = 0;

        // Prime the pipeline. Every channel has to be on the wire before the
        // loop starts waiting for the oldest one, or it would wait out the
        // response timeout for a frame that was never sent.
        for (unsigned i = 0; i < pipeline_depth_; ++i)
            send_channel(i);

        unsigned index = 0;
        while (!stop_.load(std::memory_order_relaxed)) {
            // A point-to-point link preserves order and the slave forwards in
            // order, so the channel sent longest ago is the one that returns
            // first: round-robin is the correct completion order.
            const PdChannel& channel = channels_[index];
            const ec_domain_state_t ds = wait_for_channel(index);
            total_cycles_++;
            window_cycles++;

            bool delivered = false;
            if (ds.wc_state == expected_wc_) {
                wc_error_streak = 0;
                if (process_data_discontinuity_ && !channel_zero_probe_[index]) {
                    // In flight since before the discontinuity: its outputs are
                    // a pre-error generation, so it settles nothing. Withhold
                    // delivery (the ARQ simply keeps the ack, and the slave
                    // re-presents the chunk) and let the reissued zero probe
                    // classify the peer.
                } else if (process_data_discontinuity_) {
                    // Sequence zero is reserved by pd_stream. Once a session
                    // has been established, peer ack=0 after an incomplete WKC
                    // is therefore a reset signature even if the cached AL
                    // state changed SAFEOP -> OP too quickly to observe.
                    const bool peer_reset =
                        slave_left_op_
                        || static_cast<std::uint8_t>(channel.stream_inputs[1]) == 0;
                    if (peer_reset) {
                        reset_endpoint();
                        transmit_ring_.clear();
                        clear_cyclic_outputs(true);
                        for (unsigned i = 0; i < pipeline_depth_; ++i)
                            std::memset(channels_[i].outputs, 0, process_data_size());
                        cyclic_input_seq_.fill(0);
                        slave_left_op_ = false;
                        notify_link_restart();
                        logger_.warn(
                            "EtherCAT slave back to OP; stream endpoint and session reset");
                    }
                    // Skip delivery from the zero-image probe. On a mere frame
                    // drop the existing ARQ state resumes next cycle; after a
                    // peer reset the Handler's keepalive thread first sends a
                    // fresh SESSION_START.
                    process_data_discontinuity_ = false;
                } else {
                    deliver_cyclic_inputs(channel);
                    if (receive_callback_registered_.load(std::memory_order_acquire)) {
                        CallbackSink sink{.callback = receive_callback_, .delivered = delivered};
                        consume_stream_input(channel, sink);
                    }
                }
            } else {
                // Treat the first incomplete hybrid cycle as a control-path
                // discontinuity. A brief SAFEOP round trip can reset the
                // firmware's sequence history and recover before the slower
                // AL-state supervisor observes it; retaining the old image in
                // that window would make the next OP frame look fresh. Losing
                // one cyclic command on a transient drop is safer than
                // replaying a pre-drop command.
                if (!process_data_discontinuity_) {
                    if (hybrid_mode_) {
                        clear_cyclic_outputs(false);
                        // Every channel, not just this one: the others are
                        // already in flight or about to be, and a stale fixed
                        // region must not survive the discontinuity on any of
                        // them.
                        for (unsigned i = 0; i < pipeline_depth_; ++i)
                            std::memset(channels_[i].outputs, 0, ecat::kHybridMailboxRegionSize);
                    }
                }
                process_data_discontinuity_ = true;
                observe_slave_state();
                total_wc_errors_++;
                window_wc_errors++;
                ++wc_error_streak;
                if (wc_error_streak == kWcErrorReportThreshold) {
                    // The ARQ keeps the stream intact across dropped cycles;
                    // log once per streak so a broken link is visible.
                    logger_.warn(
                        "EtherCAT working counter incomplete (wc_state {}) for {} consecutive "
                        "cycles",
                        static_cast<unsigned>(ds.wc_state), wc_error_streak);
                }
            }

            // Response window. An application answering a just-delivered chunk
            // needs about a microsecond to serialize and push, but the next
            // build_own_chunk() runs nanoseconds after the callback returns --
            // the answer misses the poll it could have ridden and waits out a
            // FULL cycle. Spinning here briefly lets the answer catch this
            // cycle: request/response traffic saves ~one cycle of RTT, pure
            // downlink streams never pay (no delivery, no spin).
            if (delivered && transmit_ring_.empty()) {
                const Clock::time_point response_deadline = Clock::now() + kResponseWindow;
                while (transmit_ring_.empty() && Clock::now() < response_deadline) {}
            }

            // Diagnostic pacing spaces out the SENDS, so it lands here rather
            // than at the top of the loop. With a pipeline it paces each frame
            // individually (the interval is `pace`, not `pace / depth`), which
            // is why the quantization sweep in ecat/DESIGN.md 4.3 is only
            // meaningful at depth 1.
            if (pace.count() != 0) {
                while (Clock::now() < next_cycle) {}
                next_cycle += pace;
            }

            // Put this channel straight back on the wire and move on to the one
            // that has been out longest.
            send_channel(index);
            index = index + 1 == pipeline_depth_ ? 0 : index + 1;

            // Achieved frame rate is THE latency diagnostic (end-to-end RTT is a
            // small multiple of the frame interval). The clock is only sampled
            // every 1024 frames to keep the hot loop clean.
            if ((window_cycles & 0x3FFU) == 0) {
                const Clock::time_point now = Clock::now();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start);
                if (elapsed >= kStatsInterval) {
                    logger_.info(
                        "EtherCAT (IgH) frame rate: {:.1f} kHz at pipeline depth {} ({} wc errors "
                        "in the last {} s)",
                        static_cast<double>(window_cycles) / static_cast<double>(elapsed.count()),
                        pipeline_depth_, window_wc_errors, elapsed.count() / 1000);
                    window_start = now;
                    window_cycles = 0;
                    window_wc_errors = 0;
                }
            }
        }
    }

    // ecrt_slave_config_state() is RT-safe and reads state cached by the master
    // FSM. Call it only on incomplete-WKC cycles, but on every one of them: a
    // short SAFEOP round trip must not wait for a long error streak. The peer
    // ack=0 probe in cycle_loop covers a transition faster than this cache.
    void observe_slave_state() {
        ec_slave_config_state_t slave_state{};
        if (ecrt_slave_config_state(slave_config_, &slave_state) != 0)
            return;
        if (!slave_state.online || !slave_state.operational
            || slave_state.al_state != kAlStateOp) {
            if (slave_left_op_)
                return;

            ec_master_state_t ms{};
            ecrt_master_state(master_, &ms);
            // The firmware restarts all fixed seq gates on SAFEOP -> OP. Drop
            // both the pending image and local generations now; the cycle loop
            // suppresses process data until a complete probe returns.
            clear_cyclic_outputs(true);
            slave_left_op_ = true;
            logger_.warn(
                "EtherCAT slave left OP (online={} operational={} al_state=0x{:X}/{} "
                "link_up={} slaves_responding={} master_al_states=0x{:02X}); waiting for the "
                "master FSM to recover it",
                static_cast<unsigned>(slave_state.online),
                static_cast<unsigned>(slave_state.operational),
                static_cast<unsigned>(slave_state.al_state), al_state_name(slave_state.al_state),
                static_cast<unsigned>(ms.link_up), static_cast<unsigned>(ms.slaves_responding),
                static_cast<unsigned>(ms.al_states));
        }
    }

    // Post-delivery wait for the application's answer; a multiple of the
    // observed callback-to-transmit turnaround (~1us), still small against the
    // ~45us poll cycle it saves.
    static constexpr std::chrono::microseconds kResponseWindow{3};
    // Per-cycle stop-and-wait deadline: if the frame has not returned by here
    // the cycle is treated as dropped (wc error) and the loop moves on.
    static constexpr std::chrono::microseconds kResponseTimeout{5000};
    // Warm-up bound for the PREOP -> OP walk. Generous on purpose: the first
    // activation after `ethercatctl start` re-reads the slave's SII through
    // the (slow) EEPROM emulation path, and observed runs progressed
    // PREOP -> SAFEOP -> OP across successive 5 s windows instead of failing.
    static constexpr std::chrono::seconds kOperationalTimeout{20};

    static constexpr int kWcErrorReportThreshold = 16;
    static constexpr std::chrono::milliseconds kStatsInterval{5000};
    static constexpr std::size_t kBufferPoolLimit = 64;

    logging::Logger& logger_;

    ec_master_t* master_ = nullptr;
    ec_slave_config_t* slave_config_ = nullptr;
    std::array<PdChannel, kMaxPipelineDepth> channels_{};
    unsigned pipeline_depth_ = 1;
    uint8_t expected_wc_ = EC_WC_COMPLETE;
    bool hybrid_mode_ = false;

    ecat::PdStreamEndpoint endpoint_;
    ecat::HybridPdStreamEndpoint hybrid_endpoint_;
    LockedByteRing transmit_ring_;

    // Initial ownership: consumer=0, atomic middle=1, producer=2. The low two
    // atomic bits hold the middle-bank index; kCyclicBankDirty marks a complete
    // unpublished snapshot. Only producers take cyclic_producer_mutex_.
    std::mutex cyclic_producer_mutex_;
    std::array<CyclicBatch, kCyclicBankCount> cyclic_banks_{};
    std::uint32_t cyclic_producer_bank_ = 2;
    std::uint32_t cyclic_consumer_bank_ = 0;
    std::atomic<std::uint32_t> cyclic_middle_state_{1};
    std::array<std::byte, ecat::kHybridMailboxRegionSize> cyclic_output_image_{};
    std::array<std::uint8_t, ecat::kHybridMailboxCount> cyclic_output_seq_{};
    std::array<std::uint8_t, ecat::kHybridMailboxCount> cyclic_input_seq_{};

    // Cycle-thread-only state (no synchronization needed).
    uint64_t total_cycles_ = 0;
    uint64_t total_wc_errors_ = 0;
    bool slave_left_op_ = false;
    bool process_data_discontinuity_ = false;
    std::array<bool, kMaxPipelineDepth> channel_zero_probe_{};

    std::mutex buffer_pool_mutex_;
    std::vector<std::unique_ptr<TransportBuffer>> buffer_pool_;

    std::mutex receive_callback_mutex_;
    std::function<void(std::span<const std::byte>)> receive_callback_;
    std::atomic<bool> receive_callback_registered_{false};

    std::function<void(data::DataId, const data::CanDataView&)> cyclic_receive_callback_;
    std::atomic<bool> cyclic_receive_callback_registered_{false};

    std::function<void()> link_restart_callback_;
    std::atomic<bool> link_restart_callback_registered_{false};

    std::atomic<bool> stop_{false};
    std::thread cycle_thread_;
};

} // namespace

std::unique_ptr<Transport>
    create_transport(std::string_view interface_name, const ConnectionOptions& options) {
    return std::make_unique<Igh>(interface_name, options);
}

} // namespace librmcs::host::transport::igh

#endif // LIBRMCS_ENABLE_IGH
