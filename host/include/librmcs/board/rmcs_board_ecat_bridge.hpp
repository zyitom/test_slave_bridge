#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/board/common.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>

namespace librmcs::board {

// Board interface for the rmcs_board EtherCAT stream bridge
// (firmware/rmcs_board/ecat on the HPM6E80IVM1 / hpm6e00evk-equivalent
// hardware). Connects over an EtherCAT network interface instead of USB;
// everything above the transport -- session, data ids, callbacks -- matches
// the USB boards. The bridge exposes one CAN bus (CAN0 = MCAN4) and one UART
// (UART0 = UART1 on PY06/PY07); it has no IMU, GPIO channels or DBUS.
//
// Requires an SDK built with -DLIBRMCS_ENABLE_SOEM=ON (the constructor throws
// otherwise) and CAP_NET_RAW (or root) to open the raw network interface.
class RmcsBoardEcatBridge final {
public:
    class Callback : public data::DataCallback {
    public:
        virtual void can0_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }

        virtual void uart0_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }

        void accelerometer_receive_callback(
            const librmcs::data::AccelerometerDataView& data) override {
            (void)data;
        }
        void gyroscope_receive_callback(const librmcs::data::GyroscopeDataView& data) override {
            (void)data;
        }
        void temperature_receive_callback(const librmcs::data::TemperatureDataView& data) override {
            (void)data;
        }

    public:
        bool can_receive_callback(data::DataId id, const data::CanDataView& data) final {
            switch (id) {
            case data::DataId::kCan0: can0_receive_callback(data); return true;
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

    explicit RmcsBoardEcatBridge(
        std::string_view interface_name, Callback& callback = default_callback_,
        const AdvancedOptions& options = {})
        : handler_(interface_name, options, callback) {}

    RmcsBoardEcatBridge(const RmcsBoardEcatBridge&) = delete;
    RmcsBoardEcatBridge& operator=(const RmcsBoardEcatBridge&) = delete;
    RmcsBoardEcatBridge(RmcsBoardEcatBridge&&) = delete;
    RmcsBoardEcatBridge& operator=(RmcsBoardEcatBridge&&) = delete;
    ~RmcsBoardEcatBridge() = default;

    class PacketBuilder {
        friend class RmcsBoardEcatBridge;

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
    static inline Callback default_callback_{};
    host::protocol::Handler handler_;
};

} // namespace librmcs::board
