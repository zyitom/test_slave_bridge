#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <libusb.h>

#include "core/src/protocol/constant.hpp"
#include "core/src/utility/assert.hpp"
#include "host/src/logging/logging.hpp"
#include "host/src/transport/transport.hpp"
#include "host/src/transport/usb/device_scanner.hpp"
#include "host/src/transport/usb/helper.hpp"
#include "host/src/utility/final_action.hpp"
#include "host/src/utility/ring_buffer.hpp"

namespace librmcs::host::transport::usb {

class Usb : public Transport {
public:
    explicit Usb(
        uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
        const ConnectionOptions& options)
        : logger_(logging::get_logger())
        , free_transmit_transfers_(kTransmitTransferCount)
        , free_priority_transfers_(kTransmitTransferCount) {

        usb_init(usb_vid, usb_pid, serial_filter, options);
        utility::FinalAction rollback_on_failure{[this]() noexcept {
            destroy_free_transmit_transfers();
            if (priority_interface_claimed_)
                libusb_release_interface(libusb_device_handle_, kPriorityInterface);
            libusb_release_interface(libusb_device_handle_, kTargetInterface);
            libusb_close(libusb_device_handle_);
            libusb_exit(libusb_context_);
        }};

        // Opt-in uplink size instrumentation. How full each bulk transfer runs is
        // not derivable from the decoded stream -- the fields of one transfer are
        // handed to the callback individually, and their sizes say nothing about
        // how they were packed -- and usbmon needs root. So the transport counts
        // its own completion lengths when asked, and stays untouched otherwise.
        if (const char* enable = std::getenv("LIBRMCS_USB_RX_HISTOGRAM");
            enable && enable[0] == '1') {
            rx_length_histogram_ =
                std::make_unique<std::atomic<uint32_t>[]>(core::protocol::kProtocolBufferSize + 1);
        }

        // Probe dev_mem availability: if the kernel usbfs driver supports DMA-coherent
        // buffers the allocation will succeed; fall back to heap allocation silently.
        auto* probe = libusb_dev_mem_alloc(libusb_device_handle_, 1);
        if (probe) {
            libusb_dev_mem_free(libusb_device_handle_, probe, 1);
            dev_mem_available_ = true;
            logger_.info("libusb dev_mem (zero-copy) available");
        }

        init_transmit_transfers();
        if (priority_available_)
            init_priority_transmit_transfers();

        if (options.thread_setup) {
            std::atomic<bool> thread_setup_done{false};
            event_thread_ = std::thread{[this, &options, &thread_setup_done]() {
                options.thread_setup(options);
                thread_setup_done.store(true, std::memory_order_release);
                thread_setup_done.notify_one();
                handle_events();
            }};
            // Wait until thread_setup returns, so any bound options state remains alive.
            thread_setup_done.wait(false, std::memory_order_acquire);
        } else {
            event_thread_ = std::thread{[this]() { handle_events(); }};
        }

        rollback_on_failure.disable();
    }

    Usb(const Usb&) = delete;
    Usb& operator=(const Usb&) = delete;
    Usb(Usb&&) = delete;
    Usb& operator=(Usb&&) = delete;

    ~Usb() override {
        dump_rx_length_histogram();
        {
            const std::scoped_lock guard{transmit_transfer_mutex_};
            stop_handling_events_.store(true, std::memory_order::relaxed);
        }
        transmit_transfer_cv_.notify_all();
        {
            const std::scoped_lock guard{priority_transfer_mutex_};
        }
        priority_transfer_cv_.notify_all();
        destroy_free_transmit_transfers();

        // Cancel the pending receives explicitly instead of leaving it to
        // libusb_close(). Close does not reliably complete IN transfers sitting on
        // a second interface: measured, with the CAN endpoint pair claimed, 16 of
        // 16 transfers on 0x82 were still outstanding after close (6 of 16 when
        // the interface was released first), and handle_events() then spun on an
        // active_transfers_ count that could never reach zero. Cancelling before
        // close makes every one of them deliver LIBUSB_TRANSFER_CANCELLED through
        // the normal callback path, which is what decrements the count.
        {
            const std::scoped_lock guard{rx_transfer_mutex_};
            for (auto* rx : rx_transfers_)
                libusb_cancel_transfer(rx->transfer);
        }

        // ...and wait for those cancellations to actually be delivered before
        // closing the handle. Cancelling and closing back to back loses whatever
        // the event thread had not yet reaped: measured, 6 of 32 receive
        // transfers stayed outstanding that way, and handle_events() then span on
        // a count that could never reach zero. Bounded so a wedged controller
        // cannot turn teardown into a hang -- the loop below is what still
        // guarantees termination if that ever happens.
        for (int i = 0; i < kTeardownDrainRounds; i++) {
            {
                const std::scoped_lock guard{rx_transfer_mutex_};
                if (rx_transfers_.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        if (priority_interface_claimed_)
            libusb_release_interface(libusb_device_handle_, kPriorityInterface);
        libusb_release_interface(libusb_device_handle_, kTargetInterface);

        // libusb_close() reliably cancels all pending transfers and invokes their callbacks,
        // avoiding race conditions present in other cancellation methods
        libusb_close(libusb_device_handle_);

        // Guarantees join() returns even if some transfer never reports back:
        // whatever is still counted at this point can no longer be delivered,
        // because the handle it belonged to is closed.
        abandon_remaining_transfers_.store(true, std::memory_order::relaxed);

        if (event_thread_.joinable())
            event_thread_.join();

        libusb_exit(libusb_context_);
    }

    std::unique_ptr<TransportBuffer> acquire_transmit_buffer() noexcept override {
        TransferWrapper* transfer = nullptr;
        {
            std::unique_lock guard{transmit_transfer_mutex_};
            transmit_transfer_cv_.wait(guard, [this]() {
                return stop_handling_events_.load(std::memory_order::relaxed)
                    || free_transmit_transfers_.readable() != 0;
            });
            if (stop_handling_events_.load(std::memory_order::relaxed))
                return nullptr;
            free_transmit_transfers_.pop_front(
                [&transfer](TransferWrapper* value) noexcept { transfer = value; });
        }
        core::utility::assert_debug(transfer != nullptr);

        return std::unique_ptr<TransportBuffer>{transfer};
    }

    void transmit(std::unique_ptr<TransportBuffer> buffer, size_t size) override {
        core::utility::assert_debug(static_cast<bool>(buffer));

        if (size > core::protocol::kProtocolBufferSize)
            throw std::invalid_argument("Transmit size exceeds maximum transfer length");

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto& transfer = static_cast<TransferWrapper*>(buffer.get())->transfer_;
        transfer->length = static_cast<int>(size);

        int ret = libusb_submit_transfer(transfer);
        if (ret != 0) [[unlikely]] {
            throw std::runtime_error(
                std::format(
                    "Failed to submit transmit transfer: {} ({})", ret,
                    helper::libusb_errname(ret)));
        }

        // If success: Ownership is transferred to libusb
        std::ignore = buffer.release();
    }

    void release_transmit_buffer(std::unique_ptr<TransportBuffer> buffer) override {
        core::utility::assert_debug(static_cast<bool>(buffer));

        {
            const std::scoped_lock guard{transmit_transfer_mutex_};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            auto* wrapper = static_cast<TransferWrapper*>(buffer.release());
            free_transmit_transfers_.emplace_back(wrapper);
        }
        transmit_transfer_cv_.notify_one();
    }

    void receive(std::function<void(std::span<const std::byte>)> callback) override {
        if (!callback)
            throw std::invalid_argument{"Callback function cannot be null"};
        if (receive_callback_)
            throw std::logic_error{"Receive function can only be called once"};

        receive_callback_ = std::move(callback);
        init_receive_transfers(kInEndpoint, false);
    }

    bool has_priority_channel() const noexcept override { return priority_available_; }

    std::unique_ptr<TransportBuffer> acquire_priority_transmit_buffer() noexcept override {
        if (!priority_available_)
            return acquire_transmit_buffer();

        TransferWrapper* transfer = nullptr;
        {
            std::unique_lock guard{priority_transfer_mutex_};
            priority_transfer_cv_.wait(guard, [this]() {
                return stop_handling_events_.load(std::memory_order::relaxed)
                    || free_priority_transfers_.readable() != 0;
            });
            if (stop_handling_events_.load(std::memory_order::relaxed))
                return nullptr;
            free_priority_transfers_.pop_front(
                [&transfer](TransferWrapper* value) noexcept { transfer = value; });
        }
        core::utility::assert_debug(transfer != nullptr);

        return std::unique_ptr<TransportBuffer>{transfer};
    }

    // Submission is endpoint-agnostic: the endpoint was baked into the transfer
    // when its pool was built, so this is the same code as transmit().
    void transmit_priority(std::unique_ptr<TransportBuffer> buffer, size_t size) override {
        transmit(std::move(buffer), size);
    }

    void release_priority_transmit_buffer(std::unique_ptr<TransportBuffer> buffer) override {
        core::utility::assert_debug(static_cast<bool>(buffer));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto* wrapper = static_cast<TransferWrapper*>(buffer.get());
        if (!wrapper->is_priority_) {
            release_transmit_buffer(std::move(buffer));
            return;
        }
        {
            const std::scoped_lock guard{priority_transfer_mutex_};
            free_priority_transfers_.emplace_back(wrapper);
            std::ignore = buffer.release();
        }
        priority_transfer_cv_.notify_one();
    }

    void receive_priority(std::function<void(std::span<const std::byte>)> callback) override {
        if (!priority_available_)
            return;
        if (!callback)
            throw std::invalid_argument{"Callback function cannot be null"};
        if (priority_receive_callback_)
            throw std::logic_error{"Receive function can only be called once"};

        priority_receive_callback_ = std::move(callback);
        init_receive_transfers(kPriorityInEndpoint, true);
    }

private:
    class TransferWrapper : public TransportBuffer {
        friend class Usb;

    public:
        explicit TransferWrapper(Usb& self)
            : self_(self)
            , transfer_(self_.create_libusb_transfer())
            , buffer_(self_.alloc_transfer_buffer())
            , buffer_is_dev_mem_(self_.dev_mem_available_) {}

        TransferWrapper(const TransferWrapper&) = delete;
        TransferWrapper& operator=(const TransferWrapper&) = delete;
        TransferWrapper(TransferWrapper&&) = delete;
        TransferWrapper& operator=(TransferWrapper&&) = delete;

        ~TransferWrapper() override {
            if (transfer_) {
                logging::get_logger().error(
                    "USB TransferBuffer {} was destroyed externally - this is undefined behavior. "
                    "Buffers must be returned via transmit() or "
                    "release_transmit_buffer(). ",
                    static_cast<void*>(this));
                destroy();
            }
        }

        BufferSpanType data() const noexcept override {
            return BufferSpanType{
                reinterpret_cast<std::byte*>(buffer_), core::protocol::kProtocolBufferSize};
        }

        void destroy() noexcept {
            self_.free_transfer_buffer(buffer_, buffer_is_dev_mem_);
            buffer_ = nullptr;
            self_.destroy_libusb_transfer(transfer_);
            transfer_ = nullptr;
        }

    private:
        Usb& self_;

        libusb_transfer* transfer_;
        unsigned char* buffer_;
        bool buffer_is_dev_mem_;
        // Which pool this wrapper goes back to on completion. Set once at
        // construction; a buffer never migrates between channels.
        bool is_priority_ = false;
    };

    void usb_init(
        uint16_t vendor_id, int32_t product_id, std::string_view serial_filter,
        const ConnectionOptions& options) {
        if (const int ret = libusb_init(&libusb_context_); ret != 0) [[unlikely]] {
            throw std::runtime_error(
                std::format(
                    "Failed to initialize libusb: {} ({})", ret, helper::libusb_errname(ret)));
        }
        utility::FinalAction exit_libusb{[this]() noexcept { libusb_exit(libusb_context_); }};

        libusb_device_handle_ = DeviceScanner::select_device(
            libusb_context_, vendor_id, product_id, serial_filter, options);
        utility::FinalAction close_device_handle{
            [this]() noexcept { libusb_close(libusb_device_handle_); }};

        if (const int ret = libusb_claim_interface(libusb_device_handle_, kTargetInterface);
            ret != 0) [[unlikely]] {
            throw std::runtime_error(
                std::format(
                    "Failed to claim interface {}: {} ({})", kTargetInterface, ret,
                    helper::libusb_errname(ret)));
        }

        detect_and_claim_priority_interface();

        // Libusb successfully initialized
        close_device_handle.disable();
        exit_libusb.disable();
    }

    // A second vendor-class interface carrying CAN on its own bulk pair. Probed
    // from the active configuration descriptor rather than assumed, so the same
    // host binary works against a board built without the split -- there the
    // interface simply is not there and everything stays on one pipe.
    //
    // Failing to claim it is NOT fatal: falling back to the single pipe costs
    // tail latency under UART load, while refusing to open the board at all
    // would cost the user their link. It is logged either way.
    void detect_and_claim_priority_interface() {
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_active_config_descriptor(
                libusb_get_device(libusb_device_handle_), &config)
            != 0) {
            logger_.info("could not read config descriptor; CAN stays on the shared pipe");
            return;
        }
        utility::FinalAction free_config{
            [config]() noexcept { libusb_free_config_descriptor(config); }};

        bool found = false;
        for (uint8_t i = 0; i < config->bNumInterfaces && !found; i++) {
            const libusb_interface& interface = config->interface[i];
            for (int alt = 0; alt < interface.num_altsetting; alt++) {
                const libusb_interface_descriptor& descriptor = interface.altsetting[alt];
                if (descriptor.bInterfaceNumber != kPriorityInterface)
                    continue;
                if (descriptor.bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC)
                    continue;
                // Both directions must be present; a half-open pair would leave
                // one leg of the CAN path on the shared pipe, which is worse than
                // not splitting at all because it splits the ordering too.
                bool has_out = false, has_in = false;
                for (uint8_t e = 0; e < descriptor.bNumEndpoints; e++) {
                    const auto address = descriptor.endpoint[e].bEndpointAddress;
                    has_out |= (address == kPriorityOutEndpoint);
                    has_in |= (address == kPriorityInEndpoint);
                }
                found = has_out && has_in;
                break;
            }
        }

        if (!found) {
            logger_.info("board has no CAN endpoint pair; CAN shares the bulk pipe");
            return;
        }

        if (const int ret = libusb_claim_interface(libusb_device_handle_, kPriorityInterface);
            ret != 0) {
            logger_.warn(
                "found the CAN interface but could not claim it: {} ({}); "
                "CAN stays on the shared pipe",
                ret, helper::libusb_errname(ret));
            return;
        }

        priority_interface_claimed_ = true;
        priority_available_ = true;
        logger_.info("CAN has its own bulk endpoint pair (0x02/0x82)");
    }

    void init_transmit_transfers() {
        TransferWrapper* transmit_transfers[kTransmitTransferCount] = {};
        try {
            for (auto& wrapper : transmit_transfers) {
                wrapper = new TransferWrapper{*this};
                auto* transfer = wrapper->transfer_;

                libusb_fill_bulk_transfer(
                    transfer, libusb_device_handle_, kOutEndpoint, wrapper->buffer_, 0,
                    [](libusb_transfer* transfer) {
                        auto* wrapper = static_cast<TransferWrapper*>(transfer->user_data);
                        wrapper->self_.usb_transmit_complete_callback(wrapper);
                    },
                    wrapper, 0);
                transfer->flags = libusb_transfer_flags::LIBUSB_TRANSFER_ADD_ZERO_PACKET;
            }
        } catch (...) {
            for (auto& wrapper : transmit_transfers) {
                if (wrapper) {
                    wrapper->destroy();
                    delete wrapper;
                }
            }
            throw;
        }

        auto* iter = transmit_transfers;
        free_transmit_transfers_.push_back_n(
            [&iter]() noexcept { return *iter++; }, kTransmitTransferCount);
    }

    void init_priority_transmit_transfers() {
        TransferWrapper* transfers[kTransmitTransferCount] = {};
        try {
            for (auto& wrapper : transfers) {
                wrapper = new TransferWrapper{*this};
                wrapper->is_priority_ = true;
                auto* transfer = wrapper->transfer_;

                libusb_fill_bulk_transfer(
                    transfer, libusb_device_handle_, kPriorityOutEndpoint, wrapper->buffer_, 0,
                    [](libusb_transfer* transfer) {
                        auto* wrapper = static_cast<TransferWrapper*>(transfer->user_data);
                        wrapper->self_.usb_transmit_complete_callback(wrapper);
                    },
                    wrapper, 0);
                transfer->flags = libusb_transfer_flags::LIBUSB_TRANSFER_ADD_ZERO_PACKET;
            }
        } catch (...) {
            for (auto& wrapper : transfers) {
                if (wrapper) {
                    wrapper->destroy();
                    delete wrapper;
                }
            }
            throw;
        }

        auto* iter = transfers;
        free_priority_transfers_.push_back_n(
            [&iter]() noexcept { return *iter++; }, kTransmitTransferCount);
    }

    void handle_events() {
        // Bounded wait rather than libusb_handle_events(), whose internal timeout
        // is 60 s. The loop only re-tests active_transfers_ between calls, so a
        // teardown whose last completions arrive while this thread is parked in a
        // long poll leaves ~Usb() blocked in join() for that whole timeout. The
        // single-channel case happened not to hit it; adding the CAN endpoint
        // pair -- 16 more IN transfers that idle indefinitely because the board
        // only writes to them when it has CAN to send -- made it reproducible.
        //
        // This costs ten wakeups a second on an otherwise idle event thread and
        // nothing at all when traffic is flowing: an arriving completion still
        // wakes the poll immediately, exactly as before.
        static constexpr timeval kPollTimeout{.tv_sec = 0, .tv_usec = 100'000};
        while (active_transfers_.load(std::memory_order::relaxed)
               && !abandon_remaining_transfers_.load(std::memory_order::relaxed)) {
            timeval timeout = kPollTimeout;
            libusb_handle_events_timeout_completed(libusb_context_, &timeout, nullptr);
        }
    }

    // Thin wrapper for RX transfers to track buffer allocation type alongside the transfer.
    struct RxTransfer {
        Usb* self;
        libusb_transfer* transfer;
        unsigned char* buffer;
        bool is_dev_mem;
        // Which callback this transfer's data belongs to. The two channels carry
        // independent protocol streams, so their bytes must never be fed to the
        // same deserializer -- interleaving them would corrupt framing.
        bool priority;
    };

    void init_receive_transfers(unsigned char endpoint, bool priority) {
        for (size_t i = 0; i < kReceiveTransferCount; i++) {
            auto* rx = new RxTransfer{
                .self = this,
                .transfer = create_libusb_transfer(),
                .buffer = alloc_transfer_buffer(),
                .is_dev_mem = dev_mem_available_,
                .priority = priority,
            };

            libusb_fill_bulk_transfer(
                rx->transfer, libusb_device_handle_, endpoint, rx->buffer,
                static_cast<int>(core::protocol::kProtocolBufferSize),
                [](libusb_transfer* transfer) {
                    static_cast<RxTransfer*>(transfer->user_data)
                        ->self->usb_receive_complete_callback(transfer);
                },
                rx, 0);
            rx->transfer->flags = 0;

            {
                // Registered before submit: the completion callback can fire on
                // the event thread the instant submit returns, and it must find
                // itself in the list to unregister.
                const std::scoped_lock guard{rx_transfer_mutex_};
                rx_transfers_.push_back(rx);
            }

            int ret = libusb_submit_transfer(rx->transfer);
            if (ret != 0) [[unlikely]] {
                {
                    const std::scoped_lock guard{rx_transfer_mutex_};
                    std::erase(rx_transfers_, rx);
                }
                free_transfer_buffer(rx->buffer, rx->is_dev_mem);
                destroy_libusb_transfer(rx->transfer);
                delete rx;
                throw std::runtime_error(
                    std::format(
                        "Failed to submit receive transfer: {} ({})", ret,
                        helper::libusb_errname(ret)));
            }
        }
    }

    void usb_transmit_complete_callback(TransferWrapper* wrapper) {
        // Share mutex with teardown so destructor can block callbacks before draining the queue
        std::mutex& mutex = wrapper->is_priority_ ? priority_transfer_mutex_ : transmit_transfer_mutex_;
        std::condition_variable& cv =
            wrapper->is_priority_ ? priority_transfer_cv_ : transmit_transfer_cv_;
        auto& pool = wrapper->is_priority_ ? free_priority_transfers_ : free_transmit_transfers_;

        {
            const std::scoped_lock guard{mutex};

            if (stop_handling_events_.load(std::memory_order::relaxed)) [[unlikely]] {
                wrapper->destroy();
                delete wrapper;
                return;
            }

            pool.emplace_back(wrapper);
        }
        cv.notify_one();
    }

    void usb_receive_complete_callback(libusb_transfer* transfer) {
        auto* rx = static_cast<RxTransfer*>(transfer->user_data);

        if (stop_handling_events_.load(std::memory_order::relaxed)) [[unlikely]] {
            {
                // Unregister under the same lock the destructor cancels under, so
                // it can never hand libusb a transfer this callback already freed.
                const std::scoped_lock guard{rx_transfer_mutex_};
                std::erase(rx_transfers_, rx);
            }
            free_transfer_buffer(rx->buffer, rx->is_dev_mem);
            destroy_libusb_transfer(transfer);
            delete rx;
            return;
        }

        if (transfer->actual_length > 0) {
            const auto* first = reinterpret_cast<std::byte*>(transfer->buffer);
            const auto size = static_cast<std::size_t>(transfer->actual_length);
            if (rx_length_histogram_) [[unlikely]]
                rx_length_histogram_[std::min(size, core::protocol::kProtocolBufferSize)]
                    .fetch_add(1, std::memory_order::relaxed);
            (rx->priority ? priority_receive_callback_ : receive_callback_)({first, size});
        }

        int ret = libusb_submit_transfer(transfer);
        if (ret != 0) [[unlikely]] {
            if (ret == LIBUSB_ERROR_NO_DEVICE)
                logger_.error(
                    "Failed to re-submit receive transfer: Device disconnected. "
                    "Terminating...");
            else
                logger_.error(
                    "Failed to re-submit receive transfer: {} ({}). Terminating...", ret,
                    helper::libusb_errname(ret));
            free_transfer_buffer(rx->buffer, rx->is_dev_mem);
            destroy_libusb_transfer(transfer);
            delete rx;

            // TODO: Replace abrupt termination with a flag and exception-based error handling
            std::terminate();
        }
    }

    void destroy_free_transmit_transfers() noexcept {
        free_transmit_transfers_.pop_front_n([](TransferWrapper* wrapper) noexcept {
            wrapper->destroy();
            delete wrapper;
        });
        free_priority_transfers_.pop_front_n([](TransferWrapper* wrapper) noexcept {
            wrapper->destroy();
            delete wrapper;
        });
    }

    // Percentiles over the raw counts, plus how the sizes land against the
    // endpoint's max packet size -- which is what decides whether a stream is
    // costing whole USB packets or riding along in one that was already going.
    void dump_rx_length_histogram() const noexcept {
        if (!rx_length_histogram_)
            return;
        uint64_t total = 0, bytes = 0;
        for (size_t size = 0; size <= core::protocol::kProtocolBufferSize; ++size) {
            const uint64_t count = rx_length_histogram_[size].load(std::memory_order::relaxed);
            total += count;
            bytes += count * size;
        }
        if (total == 0)
            return;
        const int mps = libusb_get_max_packet_size(
            libusb_get_device(libusb_device_handle_), kInEndpoint);
        fprintf(stderr, "[rx-histogram] %llu transfers, %llu bytes, mean %.1f B/transfer\n",
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(bytes),
                static_cast<double>(bytes) / static_cast<double>(total));
        const double marks[] = {0.01, 0.50, 0.90, 0.99, 1.00};
        uint64_t seen = 0;
        size_t mark = 0;
        for (size_t size = 0; size <= core::protocol::kProtocolBufferSize && mark < 5; ++size) {
            seen += rx_length_histogram_[size].load(std::memory_order::relaxed);
            while (mark < 5
                   && static_cast<double>(seen) >= marks[mark] * static_cast<double>(total)) {
                fprintf(stderr, "[rx-histogram]   p%-3.0f %4zu B", marks[mark] * 100.0, size);
                if (mps > 0)
                    fprintf(stderr, "  = %.2f x mps(%d), last packet %zu/%d B full",
                            static_cast<double>(size) / mps, mps,
                            size % static_cast<size_t>(mps), mps);
                fprintf(stderr, "\n");
                mark++;
            }
        }
    }

    unsigned char* alloc_transfer_buffer() {
        if (dev_mem_available_) {
            auto* p =
                libusb_dev_mem_alloc(libusb_device_handle_, core::protocol::kProtocolBufferSize);
            if (p)
                return p;
            dev_mem_available_ = false;
        }
        return new unsigned char[core::protocol::kProtocolBufferSize];
    }

    void free_transfer_buffer(unsigned char* buf, bool is_dev_mem) noexcept {
        if (is_dev_mem)
            libusb_dev_mem_free(libusb_device_handle_, buf, core::protocol::kProtocolBufferSize);
        else
            delete[] buf;
    }

    libusb_transfer* create_libusb_transfer() {
        auto* transfer = libusb_alloc_transfer(0);
        if (!transfer)
            throw std::bad_alloc{};
        active_transfers_.fetch_add(1, std::memory_order::relaxed);
        return transfer;
    }

    void destroy_libusb_transfer(libusb_transfer* transfer) noexcept {
        libusb_free_transfer(transfer);
        active_transfers_.fetch_sub(1, std::memory_order::relaxed);
    }

    static constexpr int kTargetInterface = 0x00;

    static constexpr unsigned char kOutEndpoint = 0x01;
    static constexpr unsigned char kInEndpoint = 0x81;

    // Second vendor interface, carrying CAN only. Presence is detected from the
    // configuration descriptor rather than assumed from a build flag, so one host
    // binary drives both a split-endpoint board and a single-pipe one.
    static constexpr int kPriorityInterface = 0x01;
    static constexpr unsigned char kPriorityOutEndpoint = 0x02;
    static constexpr unsigned char kPriorityInEndpoint = 0x82;

    static constexpr size_t kTransmitTransferCount = 64;
    // Depth of the pre-submitted async RX transfer pool. At USB 3.0 SuperSpeed
    // (5 Gbit/s) a 4-deep pool cannot hide the submit/callback round-trip and
    // caps throughput well below the link rate; 16 keeps the bulk-IN pipe full.
    // Harmless for FS/HS boards (just a little more pinned buffer memory).
    static constexpr size_t kReceiveTransferCount = 16;

    logging::Logger& logger_;

    // Null unless LIBRMCS_USB_RX_HISTOGRAM=1; indexed by completion length.
    std::unique_ptr<std::atomic<uint32_t>[]> rx_length_histogram_;

    libusb_context* libusb_context_ = nullptr;
    libusb_device_handle* libusb_device_handle_ = nullptr;

    std::thread event_thread_;

    std::atomic<int> active_transfers_ = 0;
    std::atomic<bool> stop_handling_events_ = false;
    // Set once the device handle is closed: any transfer still counted then is
    // never going to complete, so the event loop must stop waiting for it.
    std::atomic<bool> abandon_remaining_transfers_ = false;
    static constexpr int kTeardownDrainRounds = 100; // 2 ms each

    utility::RingBuffer<TransferWrapper*> free_transmit_transfers_;
    bool dev_mem_available_ = false;
    std::mutex transmit_transfer_mutex_;
    std::condition_variable transmit_transfer_cv_;

    std::function<void(std::span<const std::byte>)> receive_callback_;

    // Priority channel. Deliberately a parallel set of members rather than a
    // generalized pool: the existing single-channel paths stay byte-for-byte as
    // they were, so a board without the second interface runs exactly the code
    // it ran before.
    bool priority_available_ = false;
    bool priority_interface_claimed_ = false;
    utility::RingBuffer<TransferWrapper*> free_priority_transfers_;
    std::mutex priority_transfer_mutex_;
    std::condition_variable priority_transfer_cv_;
    std::function<void(std::span<const std::byte>)> priority_receive_callback_;

    // Every submitted receive transfer, so teardown can cancel them. Guarded
    // because the completion callback removes its own entry from the event
    // thread while the destructor walks the list from the main thread.
    std::mutex rx_transfer_mutex_;
    std::vector<RxTransfer*> rx_transfers_;
};

std::unique_ptr<Transport> create_transport(
    uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
    const ConnectionOptions& options) {
    return std::make_unique<usb::Usb>(usb_vid, usb_pid, serial_filter, options);
}

} // namespace librmcs::host::transport::usb
