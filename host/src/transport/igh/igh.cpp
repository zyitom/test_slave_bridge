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
//  * Stream reliability comes from the stop-and-wait ARQ in
//    librmcs::ecat::PdStreamEndpoint (shared with the firmware, roles are
//    symmetric), exactly as in the SOEM backend.
//  * transmit() only copies the frame into a byte ring; the cycle thread
//    drains it one PDO payload per acknowledged chunk. When the ring is full the
//    caller spins briefly -- end-to-end backpressure, frames are never dropped.
//  * Cycles that delivered payload grant the application a short response
//    window before the next poll is built, so an immediate answer rides this
//    cycle instead of the next one.
//
// Distributed clock: this slave is auto-elected as the DC reference clock, so
// the master FSM only advances to OP if application_time / reference-clock /
// slave-clock sync is fed every cycle before send(). Omitting it hangs the
// slave in PREOP or SAFEOP+ERROR forever. See DESIGN.md's "distributed clock"
// section; the ecrt call order below is the one verified in the standalone
// reference bench (reference/igh_latency_bench.cpp).

#if defined(LIBRMCS_ENABLE_IGH)

# include <algorithm>
# include <atomic>
# include <chrono>
# include <cstddef>
# include <cstdint>
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

# include <librmcs/ecat/pd_stream.hpp>

# include "core/src/protocol/constant.hpp"
# include "core/src/utility/assert.hpp"
# include "host/src/logging/logging.hpp"
# include "host/src/transport/transport.hpp"
# include "host/src/utility/final_action.hpp"

namespace librmcs::host::transport::igh {

namespace {

constexpr std::size_t kChunkSize = librmcs::ecat::kPdChunkSize;

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

std::uint64_t monotonic_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
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
    BufferSpanType data() const noexcept override { return BufferSpanType{storage_}; }

private:
    mutable std::byte storage_[core::protocol::kProtocolBufferSize];
};

class Igh final : public Transport {
public:
    Igh(std::string_view interface_name, const ConnectionOptions& options)
        : logger_(logging::get_logger()) {
        // The IgH target device is chosen at the ethercat.conf / master-index
        // level (ecrt_request_master), not by binding a socket to an interface
        // like SOEM does. interface_name is therefore advisory here: it is
        // logged for parity with the SOEM API but does not select the NIC.
        const std::string ifname{interface_name};

        master_ = ecrt_request_master(0);
        if (!master_)
            throw std::runtime_error{
                "ecrt_request_master(0) failed (is the ethercat service running? "
                "run: sudo /usr/local/sbin/ethercatctl start)"};
        utility::FinalAction release_on_failure{[this]() noexcept {
            ecrt_release_master(master_);
            master_ = nullptr;
        }};

        domain_ = ecrt_master_create_domain(master_);
        if (!domain_)
            throw std::runtime_error{"ecrt_master_create_domain failed"};

        slave_config_ = ecrt_master_slave_config(master_, 0, 0, kVendorId, kProductCode);
        if (!slave_config_)
            throw std::runtime_error{std::format(
                "ecrt_master_slave_config failed (expected vendor 0x{:08X} product 0x{:08X} "
                "at ring position 0)",
                kVendorId, kProductCode)};

        // Describe the fixed PDO mapping (12 contiguous 32-bit entries each
        // direction). The entries are only needed so ecrt can resolve the base
        // offset of each direction; the payload is then treated as one
        // 48-byte contiguous chunk, exactly like ec_slave[1].outputs/inputs
        // in the SOEM backend.
        ec_pdo_entry_info_t out_entries[kPdoEntryCount];
        ec_pdo_entry_info_t in_entries[kPdoEntryCount];
        for (unsigned i = 0; i < kPdoEntryCount; ++i) {
            out_entries[i] = {kOutputEntryIndex, static_cast<uint8_t>(i + 1), 32};
            in_entries[i] = {kInputEntryIndex, static_cast<uint8_t>(i + 1), 32};
        }
        ec_pdo_info_t pdo_out[] = {
            {kRxPdoIndex, kPdoEntryCount, out_entries}
        };
        ec_pdo_info_t pdo_in[] = {
            {kTxPdoIndex, kPdoEntryCount, in_entries}
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

        const int off_out =
            ecrt_slave_config_reg_pdo_entry(slave_config_, kOutputEntryIndex, 1, domain_, nullptr);
        const int off_in =
            ecrt_slave_config_reg_pdo_entry(slave_config_, kInputEntryIndex, 1, domain_, nullptr);
        if (off_out < 0 || off_in < 0)
            throw std::runtime_error{std::format(
                "ecrt_slave_config_reg_pdo_entry failed (out={} in={})", off_out, off_in)};
        off_out_ = static_cast<std::size_t>(off_out);
        off_in_ = static_cast<std::size_t>(off_in);

        if (ecrt_master_activate(master_))
            throw std::runtime_error{"ecrt_master_activate failed"};
        // After activate(), deactivate() must run before release() on cleanup.
        utility::FinalAction deactivate_on_failure{
            [this]() noexcept { ecrt_master_deactivate(master_); }};

        process_data_ = ecrt_domain_data(domain_);
        if (!process_data_)
            throw std::runtime_error{"ecrt_domain_data returned null after activate"};
        outputs_ = reinterpret_cast<std::byte*>(process_data_ + off_out_);
        inputs_ = reinterpret_cast<std::byte*>(process_data_ + off_in_);

        // Both sides restart from seq/ack 0 on every OP (re)entry: the slave
        // resets in APPL_StartOutputHandler(), we reset here, before driving
        // the master FSM up to OP.
        endpoint_.reset();
        std::memset(outputs_, 0, kChunkSize);

        drive_to_operational();

        expected_wc_ = EC_WC_COMPLETE;
        logger_.info(
            R"(EtherCAT (IgH) link up (interface hint "{}"): {}B chunks, vendor 0x{:08X} )"
            R"(product 0x{:08X})",
            ifname, kChunkSize, kVendorId, kProductCode);

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
                return buffer;
            }
        }
        return std::make_unique<IghBuffer>();
    }

    void transmit(std::unique_ptr<TransportBuffer> buffer, size_t payload_size) override {
        core::utility::assert_always(buffer != nullptr);
        core::utility::assert_always(payload_size <= core::protocol::kProtocolBufferSize);
        const std::span<const std::byte> payload{buffer->data().data(), payload_size};
        // Spin until the ring accepts the whole frame: the stream must stay
        // lossless, and sustained fullness means the link (44B per
        // acknowledged chunk) is saturated -- backpressure is correct then.
        while (!transmit_ring_.try_push(payload)) {
            if (stop_.load(std::memory_order_relaxed))
                return;
            std::this_thread::yield();
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

private:
    void recycle_buffer(std::unique_ptr<TransportBuffer> buffer) {
        if (!buffer)
            return;
        const std::scoped_lock guard{buffer_pool_mutex_};
        if (buffer_pool_.size() < kBufferPoolLimit)
            buffer_pool_.push_back(std::move(buffer));
    }

    // Feed one cycle to the master with the distributed-clock trio. Returns the
    // domain working-counter state observed after the frame came back (or after
    // the response deadline elapsed). Shared by the warm-up and the hot loop so
    // the ecrt call order stays in exactly one place.
    ec_domain_state_t exchange_once() noexcept {
        const std::uint64_t now_ns = monotonic_ns();
        // DC trio: required every cycle because this slave is the DC reference
        // clock; without it the FSM never reaches OP (see file header).
        ecrt_master_application_time(master_, now_ns);
        ecrt_master_sync_reference_clock_to(master_, now_ns);
        ecrt_master_sync_slave_clocks(master_);
        ecrt_domain_queue(domain_);
        ecrt_master_send(master_);

        // ecrt_master_receive() is non-blocking (it drains whatever arrived),
        // so poll it until this cycle's frame returns -- stop-and-wait, one
        // frame in flight, same semantics as SOEM's blocking ec_receive.
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + kResponseTimeout;
        ec_domain_state_t ds{};
        for (;;) {
            ecrt_master_receive(master_);
            ecrt_domain_process(domain_);
            ecrt_domain_state(domain_, &ds);
            if (ds.wc_state == EC_WC_COMPLETE)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                break; // dropped cycle; caller counts it as a wc error
        }
        return ds;
    }

    // Warm-up: drive the master's internal FSM until the slave reports OP.
    // ecrt has no explicit ec_writestate(OPERATIONAL) like SOEM: the FSM walks
    // PREOP -> SAFEOP -> OP on its own as long as the application keeps cycling
    // process data (with the DC trio). Throws on timeout.
    void drive_to_operational() {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + kOperationalTimeout;
        for (;;) {
            std::memset(outputs_, 0, kChunkSize);
            exchange_once();

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
                ecrt_domain_state(domain_, &domain_state);
                throw std::runtime_error{std::format(
                    "EtherCAT slave did not reach OPERATIONAL within {} s "
                    "(link_up={} slaves_responding={} master_al_states=0x{:02X} "
                    "slave_online={} slave_operational={} slave_al_state=0x{:X}/{} "
                    "domain_wc_state={} domain_wc={}; verify the live SII/PDO mapping is "
                    "{} bytes / {} entries per direction and check `ethercat slaves -v` or dmesg "
                    "for the AL status code)",
                    std::chrono::duration_cast<std::chrono::seconds>(kOperationalTimeout).count(),
                    static_cast<unsigned>(last.link_up),
                    static_cast<unsigned>(last.slaves_responding),
                    static_cast<unsigned>(last.al_states),
                    static_cast<unsigned>(slave_state.online),
                    static_cast<unsigned>(slave_state.operational),
                    static_cast<unsigned>(slave_state.al_state),
                    al_state_name(slave_state.al_state),
                    static_cast<unsigned>(domain_state.wc_state),
                    static_cast<unsigned>(domain_state.working_counter), kChunkSize,
                    kPdoEntryCount)};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    void cycle_loop() {
        using Clock = std::chrono::steady_clock;

        uint32_t wc_error_streak = 0;
        Clock::time_point window_start = Clock::now();
        uint64_t window_cycles = 0;
        uint64_t window_wc_errors = 0;

        while (!stop_.load(std::memory_order_relaxed)) {
            endpoint_.build_own_chunk(outputs_, transmit_ring_);
            const ec_domain_state_t ds = exchange_once();
            total_cycles_++;
            window_cycles++;

            bool delivered = false;
            if (ds.wc_state == expected_wc_) {
                wc_error_streak = 0;
                if (slave_left_op_) {
                    // Frames flow again == the FSM brought the slave back to
                    // OP. Mirror the firmware, which resets its ARQ endpoint on
                    // every SAFEOP -> OP transition: both sides restart from
                    // seq/ack 0. Bytes in flight across the outage are gone;
                    // the protocol session layer above resynchronizes. Skip
                    // delivery this cycle -- the inputs predate the reset.
                    endpoint_.reset();
                    std::memset(outputs_, 0, kChunkSize);
                    slave_left_op_ = false;
                    logger_.warn("EtherCAT slave back to OP; stream endpoint reset");
                } else if (receive_callback_registered_.load(std::memory_order_acquire)) {
                    CallbackSink sink{.callback = receive_callback_, .delivered = delivered};
                    endpoint_.on_peer_chunk(inputs_, sink);
                }
            } else {
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
                if (wc_error_streak >= kRecoveryThresholdCycles) {
                    supervise();
                    wc_error_streak = 0;
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

            // Achieved poll rate is THE latency diagnostic (frame RTT is a
            // small multiple of the cycle period). The clock is only sampled
            // every 1024 cycles to keep the hot loop clean.
            if ((window_cycles & 0x3FFU) == 0) {
                const Clock::time_point now = Clock::now();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start);
                if (elapsed >= kStatsInterval) {
                    logger_.info(
                        "EtherCAT (IgH) cycle rate: {:.1f} kHz ({} wc errors in the last {} s)",
                        static_cast<double>(window_cycles) / static_cast<double>(elapsed.count()),
                        window_wc_errors, elapsed.count() / 1000);
                    window_start = now;
                    window_cycles = 0;
                    window_wc_errors = 0;
                }
            }
        }
    }

    // AL-state supervision, called only after the working counter has been bad
    // for a while (never on the healthy path). Unlike SOEM, the ecrt master FSM
    // re-configures and re-drives a dropped slave back to OP on its own, so
    // there is nothing to write here: we only latch that the slave left OP so
    // the cycle loop resets the ARQ endpoint when frames resume (see above).
    void supervise() {
        ec_master_state_t ms{};
        ecrt_master_state(master_, &ms);
        if (!(ms.al_states & kAlStateOp) || !ms.slaves_responding) {
            if (!slave_left_op_) {
                slave_left_op_ = true;
                logger_.warn(
                    "EtherCAT slave left OP (link_up={} slaves_responding={} al_states=0x{:02X}); "
                    "waiting for the master FSM to recover it",
                    static_cast<unsigned>(ms.link_up), static_cast<unsigned>(ms.slaves_responding),
                    static_cast<unsigned>(ms.al_states));
            }
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
    // ~a few ms of consecutive bad cycles before running the (slower) state
    // supervision -- transient frame drops never trigger it.
    static constexpr int kRecoveryThresholdCycles = 256;
    static constexpr std::chrono::milliseconds kStatsInterval{5000};
    static constexpr std::size_t kBufferPoolLimit = 64;

    logging::Logger& logger_;

    ec_master_t* master_ = nullptr;
    ec_slave_config_t* slave_config_ = nullptr;
    ec_domain_t* domain_ = nullptr;
    uint8_t* process_data_ = nullptr;
    std::size_t off_out_ = 0;
    std::size_t off_in_ = 0;
    std::byte* outputs_ = nullptr;
    std::byte* inputs_ = nullptr;
    uint8_t expected_wc_ = EC_WC_COMPLETE;

    librmcs::ecat::PdStreamEndpoint endpoint_;
    LockedByteRing transmit_ring_;

    // Cycle-thread-only state (no synchronization needed).
    uint64_t total_cycles_ = 0;
    uint64_t total_wc_errors_ = 0;
    bool slave_left_op_ = false;

    std::mutex buffer_pool_mutex_;
    std::vector<std::unique_ptr<TransportBuffer>> buffer_pool_;

    std::mutex receive_callback_mutex_;
    std::function<void(std::span<const std::byte>)> receive_callback_;
    std::atomic<bool> receive_callback_registered_{false};

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
