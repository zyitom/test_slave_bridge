#include "librmcs/protocol/handler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "core/src/protocol/deserializer.hpp"
#include "core/src/protocol/protocol.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "host/src/logging/logging.hpp"
#include "host/src/protocol/stream_buffer.hpp"
#include "host/src/transport/transport.hpp"
#include "librmcs/board/common.hpp"
#include "librmcs/data/datas.hpp"

namespace librmcs::host::protocol {

class Handler::Impl : public core::protocol::DeserializeCallback {
public:
    static constexpr auto kSessionAckTimeout = std::chrono::milliseconds{200};
    static constexpr size_t kSessionAckRetryCount = 5;
    static constexpr auto kSessionRefreshInterval = std::chrono::milliseconds{250};

    explicit Impl(std::unique_ptr<transport::Transport> transport, data::DataCallback& callback)
        : callback_(callback)
        , deserializer_(*this)
        , can_deserializer_(*this)
        , expected_session_nonce_(generate_session_nonce())
        , expected_session_start_ack_(make_session_start_ack(expected_session_nonce_))
        , transport_(std::move(transport)) {
        // USB bulk completions and EtherCAT callbacks are both arbitrary
        // slices of one reliable byte stream, not protocol-field boundaries.
        transport_->receive([this](std::span<const std::byte> buffer) { receive_stream(buffer); });
        // The CAN pipe, when the board has one, is a separate reliable byte
        // stream and needs its own deserializer: interleaving its bytes with the
        // bulk stream's would corrupt both. It carries no session control, so it
        // needs none of receive_stream()'s session-start-ack window -- the board
        // does not put a CAN field on the wire before the session is up.
        if (transport_->has_priority_channel()) {
            transport_->receive_priority(
                [this](std::span<const std::byte> buffer) { can_deserializer_.feed(buffer); });
        }
        transport_->receive_cyclic_can([this](data::DataId id, const data::CanDataView& data) {
            (void)can_deserialized_callback(id, data);
        });
        transport_->on_link_restart([this] {
            // A new ARQ generation cannot continue a partially received
            // protocol field. The callback runs on the transport receive
            // thread, so it is serialized with deserializer_.feed().
            deserializer_.finish_transfer();
            can_deserializer_.finish_transfer();
            awaiting_session_start_ack_ = true;
            session_start_ack_window_size_ = 0;
            {
                // Pair the state change with the condition-variable mutex so
                // the keepalive thread cannot miss the restart notification.
                const std::scoped_lock guard{session_mutex_};
                session_established_.store(false, std::memory_order_release);
            }
            session_cv_.notify_all();
        });

        establish_session();
        keepalive_thread_ = std::thread{[this] { keepalive_loop(); }};
    }

    ~Impl() override {
        stop_keepalive_.store(true, std::memory_order_relaxed);
        session_cv_.notify_all();
        if (keepalive_thread_.joinable())
            keepalive_thread_.join();

        transport_.reset();
    }

    // The constructor hands three `this`-capturing lambdas to transport_, and the
    // destructor joins a thread and releases that transport. Copying or moving an
    // Impl would leave those registered callbacks pointing at the old object and
    // duplicate ownership of both the thread and the transport. Nothing does so
    // today -- Handler holds Impl by raw pointer and moves the pointer, not the
    // object -- so deleting these turns a latent footgun into a compile error.
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    PacketBuilder start_transmit(bool cyclic) { return PacketBuilder{transport_.get(), cyclic}; }

    bool can_deserialized_callback(
        core::protocol::FieldId id, const data::CanDataView& data) override {
        if (!session_established())
            return true;
        if (!callback_.can_receive_callback(id, data)) {
            logging::get_logger().error("Unexpected can field id: ", static_cast<int>(id));
            return false;
        }
        return true;
    }

    bool uart_deserialized_callback(
        core::protocol::FieldId id, const data::UartDataView& data) override {
        if (!session_established())
            return true;
        if (!callback_.uart_receive_callback(id, data)) {
            logging::get_logger().error("Unexpected uart field id: ", static_cast<int>(id));
            return false;
        }
        return true;
    }

    // UART config is a downlink-only channel: the host emits it, boards never
    // send it back. Receiving one on the uplink means the peer is confused, so
    // report it as a routing error rather than silently ignoring it.
    bool uart_config_deserialized_callback(
        core::protocol::FieldId id, const data::UartConfigView& data) override {
        (void)data;
        if (!session_established())
            return true;
        logging::get_logger().error(
            "Unexpected uart config field on uplink: ", static_cast<int>(id));
        return false;
    }

    bool gpio_digital_data_deserialized_callback(
        uint8_t channel_index, const data::GpioDigitalDataView& data) override {
        if (!session_established())
            return true;
        if (!callback_.gpio_digital_read_result_callback(channel_index, data)) {
            logging::get_logger().error(
                "Unexpected gpio channel index: ", static_cast<int>(channel_index));
            return false;
        }
        return true;
    }

    bool gpio_analog_data_deserialized_callback(
        uint8_t channel_index, const data::GpioAnalogDataView& data) override {
        if (!session_established())
            return true;
        if (!callback_.gpio_analog_read_result_callback(channel_index, data)) {
            logging::get_logger().error(
                "Unexpected gpio channel index: ", static_cast<int>(channel_index));
            return false;
        }
        return true;
    }

    bool gpio_digital_read_config_deserialized_callback(
        uint8_t channel_index, const data::GpioReadConfigView& data) override {
        if (!session_established())
            return true;
        (void)channel_index;
        (void)data;
        logging::get_logger().error("Unexpected gpio digital read config field in uplink");
        return false;
    }

    bool gpio_analog_read_config_deserialized_callback(
        uint8_t channel_index, const data::GpioReadConfigView& data) override {
        if (!session_established())
            return true;
        (void)channel_index;
        (void)data;
        logging::get_logger().error("Unexpected gpio analog read config field in uplink");
        return false;
    }

    void accelerometer_deserialized_callback(const data::ImuAccelerometerDataView& data) override {
        if (!session_established())
            return;
        callback_.accelerometer_receive_callback(data);
    }

    void gyroscope_deserialized_callback(const data::ImuGyroscopeDataView& data) override {
        if (!session_established())
            return;
        callback_.gyroscope_receive_callback(data);
    }

    void temperature_deserialized_callback(const data::ImuTemperatureDataView& data) override {
        if (!session_established())
            return;
        callback_.temperature_receive_callback(data);
    }

    void session_control_deserialized_callback(const data::SessionControlView& data) override {
        if (data.nonce != expected_session_nonce_)
            return;

        bool notify = false;
        {
            const std::scoped_lock guard{session_mutex_};
            switch (data.type) {
            case data::SessionType::kStartAck:
                session_established_.store(true, std::memory_order_release);
                ++session_start_ack_count_;
                notify = true;
                break;
            case data::SessionType::kKeepaliveAck:
                ++session_keepalive_ack_count_;
                notify = true;
                break;
            default: break;
            }
        }
        if (notify)
            session_cv_.notify_all();
    }

    void error_callback() override {
        logging::get_logger().error("Deserializer encountered an error while parsing input");
    }

private:
    static constexpr size_t kSessionStartAckSize = sizeof(core::protocol::FieldHeaderExtended)
                                                 + sizeof(core::protocol::SessionHeader)
                                                 - sizeof(core::protocol::FieldHeader);

    static std::array<std::byte, kSessionStartAckSize> make_session_start_ack(uint32_t nonce) {
        std::array<std::byte, kSessionStartAckSize> ack{};
        core::protocol::FieldHeaderExtended::Ref field{ack.data()};
        field.set<core::protocol::FieldHeaderExtended::Id>(core::protocol::FieldId::kExtend);
        field.set<core::protocol::FieldHeaderExtended::IdExtended>(
            core::protocol::FieldId::kSession);

        core::protocol::SessionHeader::Ref session{
            ack.data() + sizeof(core::protocol::FieldHeaderExtended)
            - sizeof(core::protocol::FieldHeader)};
        session.set<core::protocol::SessionHeader::Type>(data::SessionType::kStartAck);
        session.set<core::protocol::SessionHeader::Nonce>(nonce);
        return ack;
    }

    // A reopened transport may still complete queued bytes from the previous
    // session. Only this Handler's nonce identifies a safe field boundary.
    void receive_stream(std::span<const std::byte> buffer) {
        if (!awaiting_session_start_ack_) {
            deserializer_.feed(buffer);
            return;
        }

        for (size_t i = 0; i < buffer.size(); ++i) {
            if (session_start_ack_window_size_ < session_start_ack_window_.size()) {
                session_start_ack_window_[session_start_ack_window_size_++] = buffer[i];
            } else {
                std::ranges::move(
                    session_start_ack_window_.begin() + 1, session_start_ack_window_.end(),
                    session_start_ack_window_.begin());
                session_start_ack_window_.back() = buffer[i];
            }

            if (session_start_ack_window_size_ != session_start_ack_window_.size()
                || session_start_ack_window_ != expected_session_start_ack_) {
                continue;
            }

            awaiting_session_start_ack_ = false;
            deserializer_.feed(session_start_ack_window_);
            deserializer_.feed(buffer.subspan(i + 1));
            return;
        }
    }

    [[nodiscard]] bool session_established() const {
        return session_established_.load(std::memory_order_acquire);
    }

    void establish_session() {
        for (size_t attempt = 0; attempt < kSessionAckRetryCount; ++attempt) {
            uint64_t previous_session_start_ack_count = 0;
            {
                const std::scoped_lock guard{session_mutex_};
                previous_session_start_ack_count = session_start_ack_count_;
            }

            send_session_start();

            std::unique_lock lock{session_mutex_};
            if (session_cv_.wait_for(
                    lock, kSessionAckTimeout, [this, previous_session_start_ack_count] {
                        return stop_keepalive_.load(std::memory_order_relaxed)
                            || session_start_ack_count_ > previous_session_start_ack_count;
                    })) {
                if (stop_keepalive_.load(std::memory_order_relaxed))
                    return;
                return;
            }
        }

        throw std::runtime_error{"Timed out waiting for SESSION_ACK"};
    }

    void send_session_start() { send_session_control(data::SessionType::kStart, "Session Start"); }

    void refresh_session() {
        for (size_t attempt = 0; attempt < kSessionAckRetryCount; ++attempt) {
            if (!session_established())
                return;

            uint64_t previous_session_keepalive_ack_count = 0;
            {
                const std::scoped_lock guard{session_mutex_};
                previous_session_keepalive_ack_count = session_keepalive_ack_count_;
            }

            send_session_keepalive();

            std::unique_lock lock{session_mutex_};
            if (session_cv_.wait_for(
                    lock, kSessionAckTimeout, [this, previous_session_keepalive_ack_count] {
                        return stop_keepalive_.load(std::memory_order_relaxed)
                            || !session_established()
                            || session_keepalive_ack_count_ > previous_session_keepalive_ack_count;
                    })) {
                return;
            }
        }

        throw std::runtime_error{"Timed out waiting for SESSION_KEEPALIVE_ACK"};
    }

    void send_session_keepalive() {
        send_session_control(data::SessionType::kKeepalive, "Session Keepalive");
    }

    void send_session_control(data::SessionType type, std::string_view operation_name) {
        core::protocol::Serializer::SerializeResult result;
        {
            StreamBuffer buffer{*transport_};
            core::protocol::Serializer serializer{buffer};
            result =
                serializer.write_session_control({.type = type, .nonce = expected_session_nonce_});
        }

        core::utility::assert_debug(
            result != core::protocol::Serializer::SerializeResult::kInvalidArgument);
        if (result == core::protocol::Serializer::SerializeResult::kBadAlloc) [[unlikely]]
            throw std::runtime_error(
                std::string{"Failed to transmit "} + std::string{operation_name}
                + ": Transmit buffer unavailable (acquire failed)");
    }

    void keepalive_loop() {
        while (!stop_keepalive_.load(std::memory_order_relaxed)) {
            {
                std::unique_lock lock{session_mutex_};
                (void)session_cv_.wait_for(lock, kSessionRefreshInterval, [this] {
                    return stop_keepalive_.load(std::memory_order_relaxed)
                        || !session_established();
                });
            }
            if (stop_keepalive_.load(std::memory_order_relaxed))
                break;

            try {
                if (session_established())
                    refresh_session();
                else
                    establish_session();
            } catch (const std::exception& exception) {
                logging::get_logger().error(
                    "Failed to refresh session: {}. Terminating...", exception.what());
                std::terminate();
            }
        }
    }

    static uint32_t generate_session_nonce() {
        std::random_device random_device;
        std::uniform_int_distribution<uint32_t> distribution;
        return distribution(random_device);
    }

    data::DataCallback& callback_;
    core::protocol::Deserializer deserializer_;
    core::protocol::Deserializer can_deserializer_;

    mutable std::mutex session_mutex_;
    std::condition_variable session_cv_;
    std::atomic<bool> session_established_{false};
    uint64_t session_start_ack_count_ = 0;
    uint64_t session_keepalive_ack_count_ = 0;
    uint32_t expected_session_nonce_ = 0;
    std::array<std::byte, kSessionStartAckSize> expected_session_start_ack_{};
    std::array<std::byte, kSessionStartAckSize> session_start_ack_window_{};
    size_t session_start_ack_window_size_ = 0;
    bool awaiting_session_start_ack_ = true;

    std::unique_ptr<transport::Transport> transport_;

    std::atomic<bool> stop_keepalive_{false};
    std::thread keepalive_thread_;
};

namespace {

struct PacketBuilderImpl {
    // The CAN pipe is only split off when the transport actually has a second
    // channel AND this is not a cyclic (fixed-PDO) batch. Without both, CAN keeps
    // using the main buffer, so a board with one pipe produces exactly the packets
    // it always did -- splitting unconditionally would turn a single CAN+UART
    // packet into two on hardware that gains nothing from it.
    explicit PacketBuilderImpl(transport::Transport& transport, bool cyclic) noexcept
        : buffer_(transport, cyclic)
        , serializer_(buffer_)
        , split_can_(!cyclic && transport.has_priority_channel())
        , can_buffer_(transport, false, split_can_)
        , can_serializer_(can_buffer_) {}

    PacketBuilderImpl(PacketBuilderImpl&& other) noexcept
        : buffer_(std::move(other.buffer_))
        , serializer_(buffer_)
        , split_can_(other.split_can_)
        , can_buffer_(std::move(other.can_buffer_))
        , can_serializer_(can_buffer_) {}

    PacketBuilderImpl& operator=(PacketBuilderImpl&&) = delete;
    PacketBuilderImpl(const PacketBuilderImpl&) = delete;
    PacketBuilderImpl& operator=(const PacketBuilderImpl&) = delete;
    ~PacketBuilderImpl() = default;

    // `write_*` returns `true` if args are valid; it never reports transport/resource issues.
    // - `kInvalidArgument` => `false` (user error)
    // - `kBadAlloc` => logged and ignored (`true`) (internal/transient)
    [[nodiscard]] bool write_can(data::DataId field_id, const data::CanDataView& view) noexcept {
        if (buffer_.strict_cyclic_can())
            return buffer_.try_stage_cyclic_can(field_id, view);
        if (buffer_.try_stage_cyclic_can(field_id, view))
            return true;
        if (split_can_)
            return process_result(can_serializer_.write_can(field_id, view));
        return process_result(serializer_.write_can(field_id, view));
    }

    [[nodiscard]] bool write_uart(data::DataId field_id, const data::UartDataView& view) noexcept {
        if (reject_non_can_cyclic_batch())
            return false;
        return process_result(serializer_.write_uart(field_id, view));
    }

    [[nodiscard]] bool
        write_uart_config(data::DataId field_id, const data::UartConfigView& view) noexcept {
        if (reject_non_can_cyclic_batch())
            return false;
        return process_result(serializer_.write_uart_config(field_id, view));
    }

    [[nodiscard]] bool write_gpio_digital_data(
        uint8_t channel_index, const data::GpioDigitalDataView& view) noexcept {
        if (reject_non_can_cyclic_batch())
            return false;
        if (view.timestamp_quarter_us.has_value()) [[unlikely]]
            return false;
        return process_result(serializer_.write_gpio_digital_value(channel_index, view));
    }

    [[nodiscard]] bool write_gpio_digital_read_config(
        uint8_t channel_index, const data::GpioReadConfigView& view) noexcept {
        if (reject_non_can_cyclic_batch())
            return false;
        return process_result(serializer_.write_gpio_digital_read_config(channel_index, view));
    }

    [[nodiscard]] bool write_gpio_analog_data(
        uint8_t channel_index, const data::GpioAnalogDataView& view) noexcept {
        if (reject_non_can_cyclic_batch())
            return false;
        return process_result(serializer_.write_gpio_analog_value(channel_index, view));
    }

    [[nodiscard]] bool
        write_imu_accelerometer(const data::ImuAccelerometerDataView& view) noexcept {
        return process_result(serializer_.write_imu_accelerometer(view));
    }

    [[nodiscard]] bool write_imu_gyroscope(const data::ImuGyroscopeDataView& view) noexcept {
        return process_result(serializer_.write_imu_gyroscope(view));
    }

private:
    bool reject_non_can_cyclic_batch() noexcept {
        if (!buffer_.strict_cyclic_can())
            return false;
        buffer_.reject_cyclic_can_batch();
        return true;
    }

    static bool process_result(core::protocol::Serializer::SerializeResult result) {
        using core::protocol::Serializer;
        if (result == Serializer::SerializeResult::kSuccess) [[likely]]
            return true;
        if (result == Serializer::SerializeResult::kBadAlloc) {
            logging::get_logger().error("Transmit buffer unavailable (acquire failed)");
            return true;
        }
        if (result == Serializer::SerializeResult::kInvalidArgument) {
            return false;
        }
        core::utility::assert_failed_debug();
    }

    StreamBuffer buffer_;
    core::protocol::Serializer serializer_;
    bool split_can_;
    StreamBuffer can_buffer_;
    core::protocol::Serializer can_serializer_;
};

} // namespace

Handler::PacketBuilder::PacketBuilder(void* transport_ptr, bool cyclic) noexcept {
    static_assert(sizeof(PacketBuilderImpl) <= sizeof(storage_));
    static_assert(alignof(PacketBuilderImpl) <= alignof(std::uintptr_t));

    auto& transport_ref = *static_cast<transport::Transport*>(transport_ptr);
    std::construct_at(reinterpret_cast<PacketBuilderImpl*>(storage_), transport_ref, cyclic);
}

Handler::PacketBuilder::~PacketBuilder() noexcept {
    std::destroy_at(std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_)));
}

bool Handler::PacketBuilder::write_can(
    data::DataId field_id, const data::CanDataView& view) noexcept {
    return std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_))->write_can(field_id, view);
}

bool Handler::PacketBuilder::write_uart(
    data::DataId field_id, const data::UartDataView& view) noexcept {
    return std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_))->write_uart(field_id, view);
}

bool Handler::PacketBuilder::write_uart_config(
    data::DataId field_id, const data::UartConfigView& view) noexcept {
    return std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_))
        ->write_uart_config(field_id, view);
}

bool Handler::PacketBuilder::write_gpio_digital_data(
    uint8_t channel_index, const data::GpioDigitalDataView& view) noexcept {
    return std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_))
        ->write_gpio_digital_data(channel_index, view);
}

bool Handler::PacketBuilder::write_gpio_digital_read_config(
    uint8_t channel_index, const data::GpioReadConfigView& view) noexcept {
    return std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_))
        ->write_gpio_digital_read_config(channel_index, view);
}

bool Handler::PacketBuilder::write_gpio_analog_data(
    uint8_t channel_index, const data::GpioAnalogDataView& view) noexcept {
    return std::launder(reinterpret_cast<PacketBuilderImpl*>(storage_))
        ->write_gpio_analog_data(channel_index, view);
}

Handler::Handler(
    uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
    const board::AdvancedOptions& options, data::DataCallback& callback)
    : impl_(new Impl(
          transport::usb::create_transport(usb_vid, usb_pid, serial_filter, options), callback)) {}

#if defined(LIBRMCS_ENABLE_SOEM) || defined(LIBRMCS_ENABLE_IGH)
namespace {

// Backend selection mirrors examples/ecat_stream_latency.cpp: the SDK builds
// against whichever EtherCAT backends were compiled in (SOEM and/or IgH);
// pick at run time with RMCS_ECAT_BACKEND=soem|igh (default: igh if
// available, else soem). For IgH the interface name is only an advisory hint
// (the master owning the NIC is chosen by the IgH configuration); for SOEM it
// is the raw interface bound with CAP_NET_RAW.
std::unique_ptr<transport::Transport> create_ethercat_transport(
    std::string_view interface_name, const board::AdvancedOptions& options) {
    const char* backend_env = std::getenv("RMCS_ECAT_BACKEND");
# if defined(LIBRMCS_ENABLE_IGH)
    const std::string_view backend = backend_env ? backend_env : "igh";
# else
    const std::string_view backend = backend_env ? backend_env : "soem";
# endif
# if defined(LIBRMCS_ENABLE_SOEM)
    if (backend == "soem")
        return transport::soem::create_transport(interface_name, options);
# endif
# if defined(LIBRMCS_ENABLE_IGH)
    if (backend == "igh")
        return transport::igh::create_transport(interface_name, options);
# endif
    throw std::runtime_error{
        "unknown or unavailable EtherCAT backend requested via RMCS_ECAT_BACKEND "
        "(compiled-in backends are selected by -DLIBRMCS_ENABLE_SOEM / "
        "-DLIBRMCS_ENABLE_IGH)"};
}

} // namespace

Handler::Handler(
    std::string_view ethercat_interface_name, const board::AdvancedOptions& options,
    data::DataCallback& callback)
    : impl_(new Impl(create_ethercat_transport(ethercat_interface_name, options), callback)) {}
#else
Handler::Handler(
    std::string_view ethercat_interface_name, const board::AdvancedOptions& options,
    data::DataCallback& callback) {
    (void)ethercat_interface_name;
    (void)options;
    (void)callback;
    throw std::runtime_error{"librmcs-sdk was built without an EtherCAT transport; "
                             "reconfigure with -DLIBRMCS_ENABLE_SOEM=ON and/or "
                             "-DLIBRMCS_ENABLE_IGH=ON"};
}
#endif

Handler::Handler(Handler&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {}

Handler& Handler::operator=(Handler&& other) noexcept {
    if (this == &other)
        return *this;
    delete impl_;
    impl_ = std::exchange(other.impl_, nullptr);
    return *this;
}

Handler::~Handler() noexcept { delete impl_; }

Handler::PacketBuilder Handler::start_transmit() noexcept {
    core::utility::assert_debug(impl_);
    return impl_->start_transmit(false);
}

Handler::PacketBuilder Handler::start_cyclic_transmit() noexcept {
    core::utility::assert_debug(impl_);
    return impl_->start_transmit(true);
}

} // namespace librmcs::host::protocol
