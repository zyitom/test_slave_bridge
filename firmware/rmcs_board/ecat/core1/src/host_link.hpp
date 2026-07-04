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
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"
#include "xcore_channel.hpp"

namespace librmcs::firmware::ecat {

// Host-facing protocol endpoint of the fieldbus core: the EtherCAT analog of
// usb::Vendor. Downlink bytes arrive from the SHARE_RAM down ring (fed by the
// core0 PD stream, which the stop-and-wait ARQ makes lossless and ordered) and
// go through the librmcs deserializer to the CAN/UART drivers; uplink frames
// are serialized into the interrupt-safe batch buffer by the driver ISRs and
// pumped into the up ring by the main loop.
//
// The session handshake (kStart nonce + keepalive lease) is byte-identical to
// the USB transport, so the host SDK works unchanged on top of the SOEM
// transport. The dispatch/session logic intentionally mirrors usb::Vendor;
// unifying both behind app/src/link is a known follow-up refactor.
class HostLink
    : private core::protocol::DeserializeCallback
    , private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<HostLink>;

    HostLink() = default;

    // 1000 ms session lease, in 0.25 us ticks (4 MHz mchtmr).
    static constexpr uint64_t kSessionLeaseQuarterUs = 4'000'000U;

    core::protocol::Serializer& serializer() { return serializer_; }

    bool session_established() const { return session_established_; }

    // Main loop, step 1: bytes popped from the down ring. The PD stream has no
    // transfer boundaries (unlike USB), so the deserializer is fed
    // continuously; framing recovery happens on link restart only.
    void handle_downlink(std::span<const std::byte> buffer) { deserializer_.feed(buffer); }

    // Main loop, step 2: core0 bumped link_epoch (SAFEOP -> OP re-entry, i.e.
    // the master restarted the PD stream). Session policy: drop the session
    // and any partially deserialized frame; the host re-handshakes on top of
    // the fresh ARQ stream, and stale uplink batches are cleared on the next
    // kStart. Ring contents are left alone (core0 owns the other end).
    void handle_link_restart() {
        deserializer_.finish_transfer();
        deactivate_session();
    }

    // Main loop, step 3: pump serialized uplink batches into the up ring.
    // XcoreRing::try_push is all-or-nothing, so a batch that does not fit
    // simply stays pending until the PD stream drains the ring (end-to-end
    // backpressure, nothing is dropped).
    void try_transmit(XcoreRing<kXcoreUpRingSize>& up_ring) {
        refresh_session_state();

        if (!session_established_)
            return;

        if (!transmitting_batch_) {
            transmitting_batch_ = transmit_buffer_.pop_batch();
            if (!transmitting_batch_)
                return;
        }

        if (up_ring.try_push(transmitting_batch_->data())) {
            link::InterruptSafeBuffer::release_batch(transmitting_batch_);
            transmitting_batch_ = nullptr;
        }
    }

    void deactivate_session() { session_established_ = false; }

private:
    void activate_session(uint32_t nonce) {
        if (transmitting_batch_) {
            link::InterruptSafeBuffer::release_batch(transmitting_batch_);
            transmitting_batch_ = nullptr;
        }
        transmit_buffer_.clear();

        current_session_nonce_ = nonce;
        last_session_refresh_ = timer::Timer::timestamp64_quarter_us();
        session_established_ = true;
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

    void accelerometer_deserialized_callback(const data::AccelerometerDataView& data) override {
        (void)data;
    }

    void gyroscope_deserialized_callback(const data::GyroscopeDataView& data) override {
        (void)data;
    }

    void temperature_deserialized_callback(const data::TemperatureDataView& data) override {
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
        // The ARQ link never corrupts bytes; a deserialization error implies a
        // host/firmware framing bug. Recovery happens on link restart.
    }

    core::protocol::Deserializer deserializer_{*this};

    link::InterruptSafeBuffer transmit_buffer_;
    core::protocol::Serializer serializer_{transmit_buffer_};

    const link::InterruptSafeBuffer::Batch* transmitting_batch_ = nullptr;
    bool session_established_ = false;
    uint32_t current_session_nonce_ = 0;
    uint64_t last_session_refresh_ = 0;
};

inline constinit HostLink::Lazy host_link;

} // namespace librmcs::firmware::ecat
