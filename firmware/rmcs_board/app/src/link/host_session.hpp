#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/deserializer.hpp"
#include "core/src/protocol/protocol.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/rmcs_board/app/src/can/can.hpp"
#include "firmware/rmcs_board/app/src/link/interrupt_safe_buffer.hpp"
#include "firmware/rmcs_board/app/src/timer/timer.hpp"
#include "firmware/rmcs_board/app/src/uart/uart.hpp"

namespace librmcs::firmware::link {

// Host-session endpoint shared by every host transport of this application:
// the librmcs session lifecycle (kStart nonce + keepalive lease) and the
// downlink dispatch to the CAN/UART drivers, together with the uplink
// serializer and its interrupt-safe batch buffer. A transport (USB vendor
// class, EtherCAT PD stream) subclasses this and keeps only its transmission
// shape: how the current batch reaches the wire.
class HostSession
    : private core::protocol::DeserializeCallback
    , private core::utility::Immovable {
public:
    HostSession() = default;

    // 1000 ms session lease, in 0.25 us/tick units (4 MHz mchtmr).
    static constexpr uint64_t kSessionLeaseQuarterUs = 4'000'000U;

    core::protocol::Serializer& serializer() { return serializer_; }

    // True once the host session handshake (kStart nonce + keepalive lease) is
    // up and data is actually being forwarded -- distinct from mere transport
    // connectivity (USB enumeration / EtherCAT OP).
    bool session_established() const { return session_established_; }

    // True once the kStart ack has been queued: the uplink stream may carry
    // driver telemetry from this point on.
    bool uplink_enabled() const { return uplink_enabled_; }

    void handle_downlink(std::span<const std::byte> buffer) { deserializer_.feed(buffer); }

    void finish_downlink_transfer() { deserializer_.finish_transfer(); }

    void deactivate_session() {
        uplink_enabled_ = false;
        session_established_ = false;
    }

protected:
    // Transport transmit path, step 1: refresh the session lease, then hand
    // out the batch currently in flight (popping the next pending one if none
    // is). Returns nullptr when the session is down or nothing is pending.
    // The batch stays current across calls until finish_batch(), so partial
    // transfers (USB packetization) and full-batch retries (ring
    // backpressure) both resume where they left off.
    const InterruptSafeBuffer::Batch* next_batch() {
        refresh_session_state();

        if (!session_established_)
            return nullptr;

        if (!transmitting_batch_)
            transmitting_batch_ = transmit_buffer_.pop_batch();
        return transmitting_batch_;
    }

    // Transport transmit path, step 2: the current batch fully reached the
    // wire; release it back to the pool.
    void finish_batch() {
        InterruptSafeBuffer::release_batch(transmitting_batch_);
        transmitting_batch_ = nullptr;
    }

    // A kStart with a new nonce reset the stream: the in-flight batch and all
    // pending batches were just dropped. Transports reset transfer progress.
    virtual void session_activated_callback() {}

private:
    void activate_session(uint32_t nonce) {
        if (transmitting_batch_) {
            InterruptSafeBuffer::release_batch(transmitting_batch_);
            transmitting_batch_ = nullptr;
        }
        transmit_buffer_.clear();
        session_activated_callback();

        current_session_nonce_ = nonce;
        last_session_refresh_ = timer::Timer::timestamp64_quarter_us();
        session_established_ = true;
        uplink_enabled_ = false;
    }

    void refresh_session_state() {
        if (!session_established_)
            return;

        if (timer::Timer::timestamp64_quarter_us() - last_session_refresh_ < kSessionLeaseQuarterUs)
            return;

        deactivate_session();
    }

    bool can_deserialized_callback(
        core::protocol::FieldId id, const data::CanDataView& data) override {
        if (!session_established_)
            return true;
        for (size_t i = 0; i < can::kCanCount; i++) {
            if (can::kCanDataIds[i] != static_cast<data::DataId>(id))
                continue;
            can::can_array[i]->handle_downlink(data);
            return true;
        }
        return false;
    }

    bool uart_deserialized_callback(
        core::protocol::FieldId id, const data::UartDataView& data) override {
        if (!session_established_)
            return true;
        for (auto& board_uart : uart::uart_array) {
            if (static_cast<core::protocol::FieldId>(board_uart->data_id()) == id) {
                board_uart->handle_downlink(data);
                return true;
            }
        }
        return false;
    }

    // NOTE: a rejected baudrate cannot be reported to the host through the return
    // value -- it means "this field id was recognised", not "the operation
    // succeeded". Echoing the config field back on the uplink does NOT work
    // either: UartConfig is a downlink-only channel by contract, and the host's
    // handler treats an uplink one as a routing error, fails the deserializer and
    // kills the link (measured: UART went from PASS to 0/320). Reporting it needs
    // a NEW uplink field, i.e. a real protocol extension. See
    // rmcs_board/AGENTS.md "已知缺口".
    bool uart_config_deserialized_callback(
        core::protocol::FieldId id, const data::UartConfigView& data) override {
        if (!session_established_)
            return true;
        for (auto& board_uart : uart::uart_array) {
            if (static_cast<core::protocol::FieldId>(board_uart->config_data_id()) == id) {
                board_uart->handle_config(data);
                return true;
            }
        }
        return false;
    }

    // This board has no GPIO application; GPIO commands from the host are ignored.
    bool gpio_digital_data_deserialized_callback(
        uint8_t channel_index, const data::GpioDigitalDataView& data) override {
        if (!session_established_)
            return true;
        (void)channel_index;
        (void)data;
        return false;
    }

    bool gpio_analog_data_deserialized_callback(
        uint8_t channel_index, const data::GpioAnalogDataView& data) override {
        if (!session_established_)
            return true;
        (void)channel_index;
        (void)data;
        return false;
    }

    bool gpio_digital_read_config_deserialized_callback(
        uint8_t channel_index, const data::GpioReadConfigView& data) override {
        if (!session_established_)
            return true;
        (void)channel_index;
        (void)data;
        return false;
    }

    bool gpio_analog_read_config_deserialized_callback(
        uint8_t channel_index, const data::GpioReadConfigView& data) override {
        if (!session_established_)
            return true;
        (void)channel_index;
        (void)data;
        return false;
    }

    void accelerometer_deserialized_callback(const data::ImuAccelerometerDataView& data) override {
        (void)data;
    }

    void gyroscope_deserialized_callback(const data::ImuGyroscopeDataView& data) override {
        (void)data;
    }

    void temperature_deserialized_callback(const data::ImuTemperatureDataView& data) override {
        (void)data;
    }

    void session_control_deserialized_callback(const data::SessionControlView& data) override {
        switch (data.type) {
        case data::SessionType::kStart: {
            const bool same_session = session_established_ && data.nonce == current_session_nonce_;

            if (!same_session)
                activate_session(data.nonce);
            else
                last_session_refresh_ = timer::Timer::timestamp64_quarter_us();

            const auto result = serializer_.write_session_control(
                {.type = data::SessionType::kStartAck, .nonce = data.nonce});
            core::utility::assert_always(
                result != core::protocol::Serializer::SerializeResult::kInvalidArgument);
            uplink_enabled_ = true;
            break;
        }
        case data::SessionType::kKeepalive:
            if (!session_established_ || data.nonce != current_session_nonce_)
                return;

            last_session_refresh_ = timer::Timer::timestamp64_quarter_us();
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
        // Every transport below this layer delivers bytes losslessly (USB
        // bulk, EtherCAT stop-and-wait ARQ), so a deserialization error
        // implies a host/firmware framing bug. Recovery happens at the next
        // transfer boundary / link restart via finish_downlink_transfer().
    }

    core::protocol::Deserializer deserializer_{*this};

    InterruptSafeBuffer transmit_buffer_;
    core::protocol::Serializer serializer_{transmit_buffer_};

    const InterruptSafeBuffer::Batch* transmitting_batch_ = nullptr;
    bool uplink_enabled_ = false;
    bool session_established_ = false;
    uint32_t current_session_nonce_ = 0;
    uint64_t last_session_refresh_ = 0;
};

} // namespace librmcs::firmware::link
