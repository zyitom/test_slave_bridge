#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/board/common.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>
#include <librmcs/spec/rmcs_board_hpm6e8y/can.hpp>
#include <librmcs/spec/rmcs_board_hpm6e8y/uart.hpp>

namespace librmcs::board {

// Board interface for the HPM6E8Y board over USB (PID 0xA904). This is the
// USB-transport twin of RmcsBoardEcatBridge: the same HPM6E8Y hardware and the
// same shared app-layer, but connected as a USB vendor device instead of an
// EtherCAT slave. It exposes all four physical CAN buses (CAN0..CAN3 =
// MCAN0..MCAN3) and one UART (UART0 = UART1); it has no IMU, GPIO channels or
// DBUS, so those callbacks are intentionally absent.
class RmcsBoardHpm6e8y final {
public:
    class Callback : public data::DataCallback {
    public:
        // Channel descriptors for this board. Addressing a channel through its
        // descriptor is what lets generic code reach data_id and, for UARTs,
        // config_data_id without a second lookup table.
        struct Spec {
            using Can = spec::rmcs_board_hpm6e8y::CanDescriptor;
            static constexpr spec::rmcs_board_hpm6e8y::internal::CanDescriptors kCans{};

            using Uart = spec::rmcs_board_hpm6e8y::UartDescriptor;
            static constexpr spec::rmcs_board_hpm6e8y::internal::UartDescriptors kUarts{};
        };

        struct View {
            using Can = data::CanDataView;
            using Uart = data::UartDataView;
            using UartConfig = data::UartConfigView;
            using ImuAccelerometer = data::ImuAccelerometerDataView;
            using ImuGyroscope = data::ImuGyroscopeDataView;
            using ImuTemperature = data::ImuTemperatureDataView;
        };

        virtual void can0_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }
        virtual void can1_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }
        virtual void can2_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }
        virtual void can3_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }

        virtual void uart0_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }

        void accelerometer_receive_callback(
            const librmcs::data::ImuAccelerometerDataView& data) override {
            (void)data;
        }
        void gyroscope_receive_callback(const librmcs::data::ImuGyroscopeDataView& data) override {
            (void)data;
        }
        void temperature_receive_callback(const librmcs::data::ImuTemperatureDataView& data) override {
            (void)data;
        }

    public:
        bool can_receive_callback(data::DataId id, const data::CanDataView& data) final {
            switch (id) {
            case data::DataId::kCan0: can0_receive_callback(data); return true;
            case data::DataId::kCan1: can1_receive_callback(data); return true;
            case data::DataId::kCan2: can2_receive_callback(data); return true;
            case data::DataId::kCan3: can3_receive_callback(data); return true;
            default: return false;
            }
        }

        bool uart_receive_callback(data::DataId id, const data::UartDataView& data) final {
            switch (id) {
            case data::DataId::kUart0: uart0_receive_callback(data); return true;
            default: return false;
            }
        }

        bool gpio_digital_read_result_callback(
            uint8_t channel_index, const data::GpioDigitalDataView& data) final {
            (void)channel_index;
            (void)data;
            return false;
        }
        bool gpio_analog_read_result_callback(
            uint8_t channel_index, const data::GpioAnalogDataView& data) final {
            (void)channel_index;
            (void)data;
            return false;
        }
    };

    explicit RmcsBoardHpm6e8y(
        Callback& callback = default_callback_, std::string_view serial_filter = {},
        const AdvancedOptions& options = {})
        : handler_(0xA11C, 0xA904, serial_filter, options, callback) {}

    RmcsBoardHpm6e8y(const RmcsBoardHpm6e8y&) = delete;
    RmcsBoardHpm6e8y& operator=(const RmcsBoardHpm6e8y&) = delete;
    RmcsBoardHpm6e8y(RmcsBoardHpm6e8y&&) = delete;
    RmcsBoardHpm6e8y& operator=(RmcsBoardHpm6e8y&&) = delete;
    ~RmcsBoardHpm6e8y() = default;

    class PacketBuilder {
        friend class RmcsBoardHpm6e8y;

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

        PacketBuilder& can2_transmit(const librmcs::data::CanDataView& data) {
            if (!builder_.write_can(data::DataId::kCan2, data)) [[unlikely]]
                throw std::invalid_argument{"CAN2 transmission failed: Invalid CAN data"};
            return *this;
        }

        PacketBuilder& can3_transmit(const librmcs::data::CanDataView& data) {
            if (!builder_.write_can(data::DataId::kCan3, data)) [[unlikely]]
                throw std::invalid_argument{"CAN3 transmission failed: Invalid CAN data"};
            return *this;
        }

        PacketBuilder& uart0_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart0, data)) [[unlikely]]
                throw std::invalid_argument{"UART0 transmission failed: Invalid UART data"};
            return *this;
        }

        // Runtime reconfiguration of UART0. Rides the same downlink stream as the
        // data above, so it is ordered against it: bytes queued earlier in this
        // batch are sent at the old baudrate, later ones at the new one.
        PacketBuilder& uart0_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart0Config, config)) [[unlikely]]
                throw std::invalid_argument{"UART0 configuration failed: Invalid UART config"};
            return *this;
        }

    private:
        explicit PacketBuilder(host::protocol::Handler& handler) noexcept
            : builder_(handler.start_transmit()) {}

        host::protocol::Handler::PacketBuilder builder_;
    };
    PacketBuilder start_transmit() noexcept { return PacketBuilder{handler_}; }

private:
    static inline Callback default_callback_{};
    host::protocol::Handler handler_;
};

} // namespace librmcs::board
