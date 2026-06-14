#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/agent/common.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>

namespace librmcs::agent {

// Agent for the HPM5321 board. Unlike lite/pro it only exposes a single CAN bus
// (CAN0) and a single UART (UART0); it has no IMU, no GPIO application channels
// and no DBUS, so those callbacks are intentionally absent.
class RmcsBoardHpm5321 : private data::DataCallback {
public:
    explicit RmcsBoardHpm5321(
        std::string_view serial_filter = {}, const AdvancedOptions& options = {})
        : handler_(0xA11C, 0xA901, serial_filter, options, *this) {}

    RmcsBoardHpm5321(const RmcsBoardHpm5321&) = delete;
    RmcsBoardHpm5321& operator=(const RmcsBoardHpm5321&) = delete;
    RmcsBoardHpm5321(RmcsBoardHpm5321&&) = delete;
    RmcsBoardHpm5321& operator=(RmcsBoardHpm5321&&) = delete;
    ~RmcsBoardHpm5321() override = default;

    class PacketBuilder {
        friend class RmcsBoardHpm5321;

    public:
        PacketBuilder& can0_transmit(const librmcs::data::CanDataView& data) {
            if (!builder_.write_can(data::DataId::kCan0, data)) [[unlikely]]
                throw std::invalid_argument{"CAN0 transmission failed: Invalid CAN data"};
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
        default: return false;
        }
    }

    virtual void can0_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }

    bool uart_receive_callback(data::DataId id, const data::UartDataView& data) final {
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
