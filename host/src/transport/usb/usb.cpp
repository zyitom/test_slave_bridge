#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

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
        , dev_mem_available_(false) {

        usb_init(usb_vid, usb_pid, serial_filter, options);
        utility::FinalAction rollback_on_failure{[this]() noexcept {
            destroy_free_transmit_transfers();
            libusb_release_interface(libusb_device_handle_, kTargetInterface);
            libusb_close(libusb_device_handle_);
            libusb_exit(libusb_context_);
        }};

        // Probe dev_mem availability: if the kernel usbfs driver supports DMA-coherent
        // buffers the allocation will succeed; fall back to heap allocation silently.
        auto* probe = libusb_dev_mem_alloc(libusb_device_handle_, 1);
        if (probe) {
            libusb_dev_mem_free(libusb_device_handle_, probe, 1);
            dev_mem_available_ = true;
            logger_.info("libusb dev_mem (zero-copy) available");
        }

        init_transmit_transfers();

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
        {
            const std::scoped_lock guard{transmit_transfer_mutex_};
            stop_handling_events_.store(true, std::memory_order::relaxed);
        }
        transmit_transfer_cv_.notify_all();
        destroy_free_transmit_transfers();

        libusb_release_interface(libusb_device_handle_, kTargetInterface);

        // libusb_close() reliably cancels all pending transfers and invokes their callbacks,
        // avoiding race conditions present in other cancellation methods
        libusb_close(libusb_device_handle_);

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
        init_receive_transfers();
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

        // Libusb successfully initialized
        close_device_handle.disable();
        exit_libusb.disable();
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

    void handle_events() {
        while (active_transfers_.load(std::memory_order::relaxed)) {
            libusb_handle_events(libusb_context_);
        }
    }

    // Thin wrapper for RX transfers to track buffer allocation type alongside the transfer.
    struct RxTransfer {
        Usb* self;
        libusb_transfer* transfer;
        unsigned char* buffer;
        bool is_dev_mem;
    };

    void init_receive_transfers() {
        for (size_t i = 0; i < kReceiveTransferCount; i++) {
            auto* rx = new RxTransfer{
                this, create_libusb_transfer(), alloc_transfer_buffer(), dev_mem_available_};

            libusb_fill_bulk_transfer(
                rx->transfer, libusb_device_handle_, kInEndpoint, rx->buffer,
                static_cast<int>(core::protocol::kProtocolBufferSize),
                [](libusb_transfer* transfer) {
                    static_cast<RxTransfer*>(transfer->user_data)
                        ->self->usb_receive_complete_callback(transfer);
                },
                rx, 0);
            rx->transfer->flags = 0;

            int ret = libusb_submit_transfer(rx->transfer);
            if (ret != 0) [[unlikely]] {
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
        const std::scoped_lock guard{transmit_transfer_mutex_};

        if (stop_handling_events_.load(std::memory_order::relaxed)) [[unlikely]] {
            wrapper->destroy();
            delete wrapper;
            return;
        }

        free_transmit_transfers_.emplace_back(wrapper);
        transmit_transfer_cv_.notify_one();
    }

    void usb_receive_complete_callback(libusb_transfer* transfer) {
        auto* rx = static_cast<RxTransfer*>(transfer->user_data);

        if (stop_handling_events_.load(std::memory_order::relaxed)) [[unlikely]] {
            free_transfer_buffer(rx->buffer, rx->is_dev_mem);
            destroy_libusb_transfer(transfer);
            delete rx;
            return;
        }

        if (transfer->actual_length > 0) {
            const auto* first = reinterpret_cast<std::byte*>(transfer->buffer);
            const auto size = static_cast<std::size_t>(transfer->actual_length);
            receive_callback_({first, size});
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

    static constexpr size_t kTransmitTransferCount = 64;
    // Depth of the pre-submitted async RX transfer pool. At USB 3.0 SuperSpeed
    // (5 Gbit/s) a 4-deep pool cannot hide the submit/callback round-trip and
    // caps throughput well below the link rate; 16 keeps the bulk-IN pipe full.
    // Harmless for FS/HS boards (just a little more pinned buffer memory).
    static constexpr size_t kReceiveTransferCount = 16;

    logging::Logger& logger_;

    libusb_context* libusb_context_ = nullptr;
    libusb_device_handle* libusb_device_handle_ = nullptr;

    std::thread event_thread_;

    std::atomic<int> active_transfers_ = 0;
    std::atomic<bool> stop_handling_events_ = false;

    utility::RingBuffer<TransferWrapper*> free_transmit_transfers_;
    bool dev_mem_available_;
    std::mutex transmit_transfer_mutex_;
    std::condition_variable transmit_transfer_cv_;

    std::function<void(std::span<const std::byte>)> receive_callback_;
};

std::unique_ptr<Transport> create_transport(
    uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
    const ConnectionOptions& options) {
    return std::make_unique<usb::Usb>(usb_vid, usb_pid, serial_filter, options);
}

} // namespace librmcs::host::transport::usb
