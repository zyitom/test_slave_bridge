#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/agent/common.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>

namespace librmcs::agent {

// Agent for the HPM5321 DualCan board (PID 0xA902). Compared to the single-CAN
// hpm5321 it adds CAN1 (MCAN3). It has no IMU, no GPIO application channels and
// no DBUS, so those callbacks are intentionally absent.
class RmcsBoardHpm5321DualCan : private data::DataCallback {
public:
    explicit RmcsBoardHpm5321DualCan(
        std::string_view serial_filter = {}, const AdvancedOptions& options = {})
        : handler_(0xA11C, 0xA902, serial_filter, options, *this) {}

    RmcsBoardHpm5321DualCan(const RmcsBoardHpm5321DualCan&) = delete;
    RmcsBoardHpm5321DualCan& operator=(const RmcsBoardHpm5321DualCan&) = delete;
    RmcsBoardHpm5321DualCan(RmcsBoardHpm5321DualCan&&) = delete;
    RmcsBoardHpm5321DualCan& operator=(RmcsBoardHpm5321DualCan&&) = delete;
    ~RmcsBoardHpm5321DualCan() override = default;

    class PacketBuilder {
        friend class RmcsBoardHpm5321DualCan;

    public:
        PacketBuilder& can0_transmit(const librmcs::data::CanDataView& data) {
            if (!builder_.write_can(data::DataId::kCan0, data)) [[unlikely]]
                throw std::invalid_argument{"CAN0 transmission failed: Invalid CAN data"};
            return *this;
        }

        PacketBuilder& can1_transmit(const librmcs::data::CanDataView& data) {
            if (!builder_.write_can(data::DataId::kCan1, data)) [[unlikely]]
                throw std::invalid_argument{"CAN1 transmission failed: Invalid CAN data"};
            return *this;
        }

        PacketBuilder& uart0_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart0, data)) [[unlikely]]
                throw std::invalid_argument{"UART0 transmission failed: Invalid UART data"};
            return *this;
        }

    private:
        explicit PacketBuilder(host::protocol::Handler& handler) noexcept
            : builder_(handler.start_transmit()) {}

        host::protocol::Handler::PacketBuilder builder_;
    };
    PacketBuilder start_transmit() noexcept { return PacketBuilder{handler_}; }

private:
    bool can_receive_callback(data::DataId id, const data::CanDataView& data) final {
        switch (id) {
        case data::DataId::kCan0: can0_receive_callback(data); return true;
        case data::DataId::kCan1: can1_receive_callback(data); return true;
        default: return false;
        }
    }

    virtual void can0_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }
    virtual void can1_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }

    bool uart_receive_callback(data::DataId id, const librmcs::data::UartDataView& data) final {
        switch (id) {
        case data::DataId::kUart0: uart0_receive_callback(data); return true;
        default: return false;
        }
    }

    virtual void uart0_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }

    void gpio_digital_read_result_callback(
        uint8_t channel_index, const librmcs::data::GpioDigitalDataView& data) final {
        (void)channel_index;
        (void)data;
    }
    void gpio_analog_read_result_callback(
        uint8_t channel_index, const librmcs::data::GpioAnalogDataView& data) final {
        (void)channel_index;
        (void)data;
    }

    void accelerometer_receive_callback(const librmcs::data::AccelerometerDataView& data) override {
        (void)data;
    }
    void gyroscope_receive_callback(const librmcs::data::GyroscopeDataView& data) override {
        (void)data;
    }
    void temperature_receive_callback(const librmcs::data::TemperatureDataView& data) override {
        (void)data;
    }

    host::protocol::Handler handler_;
};

} // namespace librmcs::agent
