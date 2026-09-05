#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/board/common.hpp>
#include <librmcs/board/rmcs_can_port.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>
#include <librmcs/spec/rmcs_board_ecat_bridge/can.hpp>
#include <librmcs/spec/rmcs_board_ecat_bridge/uart.hpp>

namespace librmcs::board {

// Board interface for the rmcs_board EtherCAT stream bridge
// (firmware/rmcs_board/ecat on the HPM6E80IVM1 / hpm6e00evk-equivalent
// hardware). Connects over an EtherCAT network interface instead of USB;
// everything above the transport -- session, data ids, callbacks -- matches
// the USB boards. The bridge exposes four CAN buses (CAN1..CAN4 = physical
// CAN1..CAN4 / MCAN0..MCAN3; see the firmware board_app for pin routing) and one
// UART (UART0 = UART1 on PY06/PY07); it has no IMU, GPIO channels or DBUS.
//
// Requires an SDK built with at least one EtherCAT backend
// (-DLIBRMCS_ENABLE_SOEM=ON and/or -DLIBRMCS_ENABLE_IGH=ON; the constructor
// throws otherwise). The backend is picked at run time via RMCS_ECAT_BACKEND=
// soem|igh (default: igh if compiled in, else soem). SOEM opens the raw
// interface directly and needs CAP_NET_RAW (or root); IgH requires the master
// kernel module to own the NIC first (`ethercatctl start`) and write access to
// /dev/EtherCAT0.
class RmcsBoardEcatBridge final {
public:
    class Callback : public data::DataCallback {
    public:
        // Channel descriptors for this board. Addressing a channel through its
        // descriptor is what lets generic code reach data_id and, for UARTs,
        // config_data_id without a second lookup table.
        struct Spec {
            using Can = spec::rmcs_board_ecat_bridge::CanDescriptor;
            static constexpr spec::rmcs_board_ecat_bridge::internal::CanDescriptors kCans{};

            using Uart = spec::rmcs_board_ecat_bridge::UartDescriptor;
            static constexpr spec::rmcs_board_ecat_bridge::internal::UartDescriptors kUarts{};
        };

        struct View {
            using Can = data::CanDataView;
            using Uart = data::UartDataView;
            using UartConfig = data::UartConfigView;
            using ImuAccelerometer = data::ImuAccelerometerDataView;
            using ImuGyroscope = data::ImuGyroscopeDataView;
            using ImuTemperature = data::ImuTemperatureDataView;
        };

        // Receives from any CAN port, named as the enclosure labels it. One
        // callback with a port argument instead of one method per port: the old
        // per-port names were 0-based, so canN_receive_callback and the
        // connector printed CANN were never the same bus.
        //
        // Ports on this board: CanPort::kCan1, CanPort::kCan2, CanPort::kCan3, CanPort::kCan4.
        virtual void can_receive(rmcs::CanPort port, const librmcs::data::CanDataView& data) {
            (void)port;
            (void)data;
        }

        virtual void uart0_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }

        void accelerometer_receive_callback(
            const librmcs::data::ImuAccelerometerDataView& data) override {
            (void)data;
        }
        void gyroscope_receive_callback(const librmcs::data::ImuGyroscopeDataView& data) override {
            (void)data;
        }
        void temperature_receive_callback(
            const librmcs::data::ImuTemperatureDataView& data) override {
            (void)data;
        }

    public:
        bool can_receive_callback(data::DataId id, const data::CanDataView& data) final {
            switch (id) {
            case data::DataId::kCan1: can_receive(rmcs::CanPort::kCan1, data); return true;
            case data::DataId::kCan2: can_receive(rmcs::CanPort::kCan2, data); return true;
            case data::DataId::kCan3: can_receive(rmcs::CanPort::kCan3, data); return true;
            case data::DataId::kCan4: can_receive(rmcs::CanPort::kCan4, data); return true;
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
        // Transmits on the CAN port named as the enclosure labels it.
        // Ports on this board: CanPort::kCan1, CanPort::kCan2, CanPort::kCan3, CanPort::kCan4.
        PacketBuilder& can_transmit(rmcs::CanPort port, const librmcs::data::CanDataView& data) {
            data::DataId id{};
            switch (port) {
            case rmcs::CanPort::kCan1: id = data::DataId::kCan1; break;
            case rmcs::CanPort::kCan2: id = data::DataId::kCan2; break;
            case rmcs::CanPort::kCan3: id = data::DataId::kCan3; break;
            case rmcs::CanPort::kCan4: id = data::DataId::kCan4; break;
            default:
                throw std::out_of_range{
                    "RmcsBoardEcatBridge: CAN port out of range (CAN1..CAN4)"};
            }
            if (!builder_.write_can(id, data)) [[unlikely]]
                throw std::invalid_argument{"CAN transmission failed: Invalid CAN data"};
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
        // The only board in this family that still configures UART in band.
        // Its transport is the EtherCAT process-data stream, which has no
        // control endpoint to move to, so the firmware keeps the kUart*Config
        // path alive for the EtherCAT-owned build. The limitation that pushed
        // every USB board onto EP0 applies here unchanged: a baudrate the
        // board's divisor solver rejects is silently ignored, because this
        // return value means "the field was recognized", never "it worked".
        PacketBuilder& uart0_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart0Config, config)) [[unlikely]]
                throw std::invalid_argument{"UART0 configuration failed: Invalid UART config"};
            return *this;
        }

    private:
        explicit PacketBuilder(host::protocol::Handler& handler) noexcept
            : builder_(handler.start_transmit()) {}

        struct CyclicTag {};
        PacketBuilder(host::protocol::Handler& handler, CyclicTag) noexcept
            : builder_(handler.start_cyclic_transmit()) {}

        host::protocol::Handler::PacketBuilder builder_;
    };
    // Whether this board's link is up, re-establishing, or gone for good.
    // kFaulted means the device disappeared: the transport refuses traffic and
    // only destroying this object and constructing a new one recovers it.
    [[nodiscard]] host::protocol::Handler::LinkState link_state() const noexcept {
        return handler_.link_state();
    }

    PacketBuilder start_transmit() noexcept { return PacketBuilder{handler_}; }

    // Explicit latest-wins batch for the 4 x 7-slot hybrid PDO. Unlike
    // start_transmit(), two batches committed before an EtherCAT cycle may
    // collapse to the newest one by design. See Handler::start_cyclic_transmit.
    PacketBuilder start_cyclic_transmit() noexcept {
        return PacketBuilder{handler_, PacketBuilder::CyclicTag{}};
    }

private:
    static inline Callback default_callback_{};
    host::protocol::Handler handler_;
};

} // namespace librmcs::board
