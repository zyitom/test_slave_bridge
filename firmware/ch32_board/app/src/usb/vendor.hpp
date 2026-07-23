#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/deserializer.hpp"
#include "core/src/protocol/protocol.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/ch32_board/app/src/can/can.hpp"
#include "firmware/ch32_board/app/src/timer/timer.hpp"
#include "firmware/ch32_board/app/src/uart/uart.hpp"
#include "firmware/ch32_board/app/src/usb/interrupt_safe_buffer.hpp"
#include "firmware/ch32_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::usb {

// Thin WCH USBSS bulk-endpoint transport, defined in vendor.cpp. These replace
// mc02's TinyUSB tud_vendor_* calls; the surrounding session/serializer logic is
// CV'd unchanged. The bulk IN (device->host uplink) and bulk OUT (host->device
// downlink) endpoints are the librmcs uplink/downlink pipes.
namespace ss {
bool tx_ready();                                   // bulk IN endpoint free?
bool tx_write(const uint8_t* data, size_t size);   // queue one IN packet
bool tx_write_zlp();                               // queue a zero-length IN packet
bool enumerated();                                 // device configured on the host?
} // namespace ss

// Ported from mc02's Vendor. Owns the uplink serializer + downlink deserializer
// and the session/keepalive state machine (all board-agnostic protocol logic).
// Only the USB reads/writes differ: WCH USBSS bulk instead of TinyUSB.
class Vendor
    : private core::protocol::DeserializeCallback
    , private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Vendor>;

    // USB 3.0 SuperSpeed bulk max packet size.
    static constexpr size_t kMaxPacketSize = 1024;
    static constexpr auto kSessionLease = std::chrono::milliseconds{1000};

    Vendor() = default;

    core::protocol::Serializer& serializer() { return serializer_; }

    void deactivate_session() { session_established_ = false; }

    // Called by the USBSS bulk-OUT completion path with the received bytes.
    void handle_downlink(std::span<const std::byte> buffer, bool finished) {
        deserializer_.feed(buffer);
        if (finished)
            deserializer_.finish_transfer();
    }

    void finish_downlink_transfer() { deserializer_.finish_transfer(); }

    bool try_transmit() {
        refresh_session_state();

        if (!session_established_)
            return false;

        if (!ss::tx_ready())
            return false;

        if (!transmitting_batch_)
            transmitting_batch_ = transmit_buffer_.pop_batch();
        if (!transmitting_batch_)
            return false;

        const auto data = transmitting_batch_->data();
        const auto target_size = std::min(data.size() - transmitted_size_, kMaxPacketSize);

        if (target_size) {
            const auto* src = reinterpret_cast<const uint8_t*>(data.data() + transmitted_size_);
            core::utility::assert_debug(ss::tx_write(src, target_size));
        } else {
            core::utility::assert_debug(ss::tx_write_zlp());
        }

        transmitted_size_ += target_size;
        if (transmitted_size_ == data.size() && target_size < kMaxPacketSize) {
            transmit_buffer_.release_batch(transmitting_batch_);
            transmitting_batch_ = nullptr;
            transmitted_size_ = 0;
        }

        return true;
    }

private:
    void activate_session(uint32_t nonce) {
        if (transmitting_batch_) {
            transmit_buffer_.release_batch(transmitting_batch_);
            transmitting_batch_ = nullptr;
            transmitted_size_ = 0;
        }
        transmit_buffer_.clear();

        current_session_nonce_ = nonce;
        last_session_refresh_ = timer::timer->timepoint();
        session_established_ = true;
    }

    bool can_deserialized_callback(
        core::protocol::FieldId id, const data::CanDataView& data) override {
        if (!session_established_)
            return true;
        switch (id) {
        case data::DataId::kCan1: can::can_array[0]->handle_downlink(data); return true;
        case data::DataId::kCan2: can::can_array[1]->handle_downlink(data); return true;
        default: return false;
        }
    }

    bool uart_deserialized_callback(
        core::protocol::FieldId id, const data::UartDataView& data) override {
        if (!session_established_)
            return true;
        switch (id) {
        case data::DataId::kUart1: uart::uart_array[0]->handle_downlink(data); return true;
        case data::DataId::kUart2: uart::uart_array[1]->handle_downlink(data); return true;
        default: return false;
        }
    }

    // No GPIO/IMU channels on this board yet: reject writes, ignore reads.
    bool gpio_digital_data_deserialized_callback(
        uint8_t, const data::GpioDigitalDataView&) override {
        return session_established_ ? false : true;
    }
    bool gpio_analog_data_deserialized_callback(
        uint8_t, const data::GpioAnalogDataView&) override {
        return session_established_ ? false : true;
    }
    bool gpio_digital_read_config_deserialized_callback(
        uint8_t, const data::GpioReadConfigView&) override {
        return session_established_ ? false : true;
    }
    bool gpio_analog_read_config_deserialized_callback(
        uint8_t, const data::GpioReadConfigView&) override {
        return session_established_ ? false : true;
    }
    void accelerometer_deserialized_callback(const data::AccelerometerDataView&) override {}
    void gyroscope_deserialized_callback(const data::GyroscopeDataView&) override {}
    void temperature_deserialized_callback(const data::TemperatureDataView&) override {}

    void session_control_deserialized_callback(const data::SessionControlView& data) override {
        switch (data.type) {
        case data::SessionType::kStart: {
            const bool same_session = session_established_ && data.nonce == current_session_nonce_;
            if (!same_session)
                activate_session(data.nonce);
            else
                last_session_refresh_ = timer::timer->timepoint();

            const auto result = serializer_.write_session_control(
                {.type = data::SessionType::kStartAck, .nonce = data.nonce});
            core::utility::assert_always(
                result != core::protocol::Serializer::SerializeResult::kInvalidArgument);
            break;
        }
        case data::SessionType::kKeepalive:
            if (!session_established_ || data.nonce != current_session_nonce_)
                return;
            last_session_refresh_ = timer::timer->timepoint();
            {
                const auto result = serializer_.write_session_control(
                    {.type = data::SessionType::kKeepaliveAck, .nonce = data.nonce});
                core::utility::assert_always(
                    result != core::protocol::Serializer::SerializeResult::kInvalidArgument);
            }
            break;
        default: return;
        }
    }

    void error_callback() override {
        // TODO: Report USB downlink deserialization errors through a dedicated path.
    }

    void refresh_session_state() {
        if (!session_established_)
            return;
        if (!timer::timer->check_expired(last_session_refresh_, kSessionLease))
            return;
        deactivate_session();
    }

    core::protocol::Deserializer deserializer_{*this};

    InterruptSafeBuffer transmit_buffer_;
    core::protocol::Serializer serializer_{transmit_buffer_};

    const InterruptSafeBuffer::Batch* transmitting_batch_ = nullptr;
    size_t transmitted_size_ = 0;
    bool session_established_ = false;
    uint32_t current_session_nonce_ = 0;
    timer::Timer::TimePoint last_session_refresh_ = timer::Timer::TimePoint::min();
};

inline constinit Vendor::Lazy vendor;

} // namespace librmcs::firmware::usb
