// EtherCAT transport: carries the librmcs byte stream over the process data
// of the rmcs_board EtherCAT bridge (firmware/rmcs_board/ecat) using SOEM as
// the master stack.
//
// The whole file compiles away unless the build enables the optional SOEM
// component (cmake -DLIBRMCS_ENABLE_SOEM=ON), so the default SDK build keeps
// zero new dependencies.
//
// Design notes:
//  * One dedicated cycle thread owns all SOEM state after construction and
//    busy-polls the slave in free-run mode: build outputs, send process data,
//    receive, feed the ARQ endpoint. Poll pacing is configurable; 0 means
//    pure busy-spin for the lowest latency (pair it with thread_setup pinning
//    the thread to an isolated core).
//  * Stream reliability comes from the stop-and-wait ARQ in
//    librmcs::ecat::PdStreamEndpoint (shared with the firmware, roles are
//    symmetric); SyncManager 3-buffer latest-wins semantics are compensated
//    there, so a lost or repeated poll never corrupts the stream.
//  * transmit() only copies the frame into a byte ring; the cycle thread
//    drains it 124 bytes per acknowledged chunk. When the ring is full the
//    caller spins briefly -- end-to-end backpressure, frames are never
//    dropped (the protocol layer has no retransmission of its own).
//  * Cycles that delivered payload to the receive callback grant the
//    application a short response window before the next poll is built, so
//    an immediate answer rides this cycle instead of the next one (a full
//    poll period saved per request/response round trip).

#if defined(LIBRMCS_ENABLE_SOEM)

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

# include <soem/ethercat.h>

# include <librmcs/ecat/pd_stream.hpp>

# include "core/src/protocol/constant.hpp"
# include "core/src/utility/assert.hpp"
# include "host/src/logging/logging.hpp"
# include "host/src/transport/transport.hpp"
# include "host/src/utility/final_action.hpp"

namespace librmcs::host::transport::soem {

namespace {

constexpr std::size_t kChunkSize = librmcs::ecat::kPdChunkSize;

// Byte ring between transmit() callers and the cycle thread. A mutex is fine
// here: the critical sections are single memcpys and the cycle thread only
// takes it once per poll.
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

class SoemBuffer final : public TransportBuffer {
public:
    BufferSpanType data() const noexcept override { return BufferSpanType{storage_}; }

private:
    mutable std::byte storage_[core::protocol::kProtocolBufferSize];
};

class Soem final : public Transport {
public:
    Soem(std::string_view interface_name, const ConnectionOptions& options)
        : logger_(logging::get_logger()) {
        const std::string ifname{interface_name};

        if (ec_init(ifname.c_str()) <= 0)
            throw std::runtime_error{std::format(
                "Failed to open EtherCAT interface \"{}\" (raw sockets need CAP_NET_RAW)", ifname)};
        utility::FinalAction close_on_failure{[]() noexcept { ec_close(); }};

        if (ec_config_init(FALSE) < 1)
            throw std::runtime_error{"No EtherCAT slave found on the network"};
        if (ec_slavecount != 1)
            throw std::runtime_error{
                std::format("Expected exactly one EtherCAT slave, found {}", ec_slavecount)};

        if (ec_config_map(static_cast<void*>(io_map_)) <= 0)
            throw std::runtime_error{"EtherCAT process data mapping failed"};
        ec_configdc(); // measures propagation delay; harmless in free-run

        ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
        if (ec_slave[0].state != EC_STATE_SAFE_OP)
            throw std::runtime_error{"EtherCAT slave did not reach SAFE-OP"};

        if (ec_slave[1].Obytes != kChunkSize || ec_slave[1].Ibytes != kChunkSize)
            throw std::runtime_error{std::format(
                "Slave process data size mismatch: outputs {} inputs {} (expected {} each); "
                "not an rmcs_stream bridge or stale SII EEPROM",
                ec_slave[1].Obytes, ec_slave[1].Ibytes, kChunkSize)};
        outputs_ = reinterpret_cast<std::byte*>(ec_slave[1].outputs);
        inputs_ = reinterpret_cast<std::byte*>(ec_slave[1].inputs);

        // Both sides restart from seq/ack 0 on every OP (re)entry: the slave
        // resets in APPL_StartOutputHandler(), we reset here, right before
        // requesting OP.
        endpoint_.reset();
        std::memset(outputs_, 0, kChunkSize);

        // One valid process data exchange is required before slaves accept OP.
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);

        ec_slave[0].state = EC_STATE_OPERATIONAL;
        ec_writestate(0);
        ec_statecheck(0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE * 4);
        if (ec_slave[0].state != EC_STATE_OPERATIONAL)
            throw std::runtime_error{std::format(
                "EtherCAT slave refused OPERATIONAL (AL status 0x{:04X})",
                ec_slave[1].ALstatuscode)};

        expected_wkc_ = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
        logger_.info(
            R"(EtherCAT link up on "{}": slave "{}", {}B chunks, expected WKC {})", ifname,
            ec_slave[1].name, kChunkSize, expected_wkc_);

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

        close_on_failure.disable();
    }

    Soem(const Soem&) = delete;
    Soem& operator=(const Soem&) = delete;
    Soem(Soem&&) = delete;
    Soem& operator=(Soem&&) = delete;

    ~Soem() override {
        stop_.store(true, std::memory_order_relaxed);
        if (cycle_thread_.joinable())
            cycle_thread_.join();
        logger_.info(
            "EtherCAT link closing: {} cycles total, {} wkc errors", total_cycles_,
            total_wkc_errors_);
        ec_slave[0].state = EC_STATE_INIT;
        ec_writestate(0);
        ec_close();
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
        return std::make_unique<SoemBuffer>();
    }

    void transmit(std::unique_ptr<TransportBuffer> buffer, size_t payload_size) override {
        core::utility::assert_always(buffer != nullptr);
        core::utility::assert_always(payload_size <= core::protocol::kProtocolBufferSize);
        const std::span<const std::byte> payload{buffer->data().data(), payload_size};
        // Spin until the ring accepts the whole frame: the stream must stay
        // lossless, and sustained fullness means the link (124B per
        // acknowledged chunk) is saturated -- backpressure is the correct
        // behavior then.
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

    void cycle_loop() {
        using Clock = std::chrono::steady_clock;

        uint32_t wkc_error_streak = 0;
        Clock::time_point window_start = Clock::now();
        uint64_t window_cycles = 0;
        uint64_t window_wkc_errors = 0;

        while (!stop_.load(std::memory_order_relaxed)) {
            endpoint_.build_own_chunk(outputs_, transmit_ring_);
            ec_send_processdata();
            const int wkc = ec_receive_processdata(EC_TIMEOUTRET);
            total_cycles_++;
            window_cycles++;

            bool delivered = false;
            if (wkc >= expected_wkc_) {
                wkc_error_streak = 0;
                if (receive_callback_registered_.load(std::memory_order_acquire)) {
                    CallbackSink sink{.callback = receive_callback_, .delivered = delivered};
                    endpoint_.on_peer_chunk(inputs_, sink);
                }
            } else {
                total_wkc_errors_++;
                window_wkc_errors++;
                ++wkc_error_streak;
                if (wkc_error_streak == kWkcErrorReportThreshold) {
                    // The ARQ keeps the stream intact across dropped cycles;
                    // log once per streak so a broken link is visible.
                    logger_.warn(
                        "EtherCAT working counter low ({} < {}) for {} consecutive cycles", wkc,
                        expected_wkc_, wkc_error_streak);
                }
                if (wkc_error_streak >= kRecoveryThresholdCycles) {
                    supervise_and_recover();
                    wkc_error_streak = 0;
                }
            }

            // Response window. An application answering a just-delivered
            // chunk needs about a microsecond to serialize and push, but the
            // next build_own_chunk() runs nanoseconds after the callback
            // returns -- the answer misses the poll it could have ridden and
            // waits out a FULL cycle. Spinning here briefly lets the answer
            // catch this cycle: request/response traffic saves ~one cycle of
            // RTT, pure downlink streams never pay (no delivery, no spin),
            // and the worst case stretches delivering cycles by the window.
            if (delivered && transmit_ring_.empty()) {
                const Clock::time_point response_deadline = Clock::now() + kResponseWindow;
                while (transmit_ring_.empty() && Clock::now() < response_deadline) {}
            }

            // Achieved poll rate is THE latency diagnostic (frame RTT is a
            // small multiple of the cycle period, see pd_stream.hpp), so make
            // it visible. The clock is only sampled every 1024 cycles to keep
            // the hot loop clean.
            if ((window_cycles & 0x3FFU) == 0) {
                const Clock::time_point now = Clock::now();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start);
                if (elapsed >= kStatsInterval) {
                    logger_.info(
                        "EtherCAT cycle rate: {:.1f} kHz ({} wkc errors in the last {} s)",
                        static_cast<double>(window_cycles) / static_cast<double>(elapsed.count()),
                        window_wkc_errors, elapsed.count() / 1000);
                    window_start = now;
                    window_cycles = 0;
                    window_wkc_errors = 0;
                }
            }
        }
    }

    // Master-side AL state supervision (the SOEM "ecatcheck" duty, folded into
    // the cycle thread): acknowledge slave errors, bring the slave back to OP,
    // reconfigure it after a link loss. Called only when the working counter
    // has been bad for a while, so it never runs on the healthy path.
    void supervise_and_recover() {
        ec_readstate();

        if (ec_slave[1].state != EC_STATE_OPERATIONAL) {
            slave_left_op_ = true;

            if (ec_slave[1].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
                logger_.warn(
                    "EtherCAT slave in SAFE-OP+ERROR (AL status 0x{:04X}), acknowledging",
                    ec_slave[1].ALstatuscode);
                ec_slave[1].state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                ec_writestate(1);
            } else if (ec_slave[1].state == EC_STATE_SAFE_OP) {
                logger_.warn("EtherCAT slave fell back to SAFE-OP, requesting OP");
                ec_slave[1].state = EC_STATE_OPERATIONAL;
                ec_writestate(1);
            } else if (ec_slave[1].state == EC_STATE_NONE) {
                // Lost (cable pull / power cycle): try to bring it back.
                if (ec_recover_slave(1, EC_TIMEOUTRET3) != 0)
                    logger_.warn("EtherCAT slave recovered after link loss, reconfiguring");
                (void)ec_reconfig_slave(1, EC_TIMEOUTRET3);
            } else {
                (void)ec_reconfig_slave(1, EC_TIMEOUTRET3);
            }

            ec_statecheck(1, EC_STATE_OPERATIONAL, EC_TIMEOUTRET3);
        }

        if (slave_left_op_ && ec_slave[1].state == EC_STATE_OPERATIONAL) {
            // The slave resets its stream ARQ endpoint on every SAFEOP -> OP
            // transition (firmware APPL_StartOutputHandler); mirror it here so
            // both sides restart from seq/ack 0. Bytes that were in flight
            // across the outage are gone -- the protocol session layer above
            // is responsible for resynchronizing.
            endpoint_.reset();
            std::memset(outputs_, 0, kChunkSize);
            slave_left_op_ = false;
            logger_.warn("EtherCAT slave back to OP; stream endpoint reset");
        }
    }

    // Post-delivery wait for the application's answer; a multiple of the
    // observed callback-to-transmit turnaround (~1us), still small against
    // the ~46us poll cycle it saves.
    static constexpr std::chrono::microseconds kResponseWindow{3};

    static constexpr int kWkcErrorReportThreshold = 16;
    // ~a few ms of consecutive bad cycles before running the (slow, blocking)
    // state supervision -- transient frame drops never trigger it.
    static constexpr int kRecoveryThresholdCycles = 256;
    static constexpr std::chrono::milliseconds kStatsInterval{5000};
    static constexpr std::size_t kBufferPoolLimit = 64;

    logging::Logger& logger_;

    // SOEM requires the IO map to outlive close; 4 KiB fits our 2x128B with
    // plenty of margin for SOEM alignment.
    char io_map_[4096]{};
    std::byte* outputs_ = nullptr;
    std::byte* inputs_ = nullptr;
    int expected_wkc_ = 0;

    librmcs::ecat::PdStreamEndpoint endpoint_;
    LockedByteRing transmit_ring_;

    // Cycle-thread-only state (no synchronization needed).
    uint64_t total_cycles_ = 0;
    uint64_t total_wkc_errors_ = 0;
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
    return std::make_unique<Soem>(interface_name, options);
}

} // namespace librmcs::host::transport::soem

#endif // LIBRMCS_ENABLE_SOEM
