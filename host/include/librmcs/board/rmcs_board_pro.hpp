#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/board/common.hpp>
#include <librmcs/board/rmcs_can_port.hpp>
#include <librmcs/board/rmcs_config.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>
#include <librmcs/spec/gpio.hpp>
#include <librmcs/spec/rmcs_board_pro/can.hpp>
#include <librmcs/spec/rmcs_board_pro/gpio.hpp>
#include <librmcs/spec/rmcs_board_pro/uart.hpp>

namespace librmcs::board {

/**
 * @brief High-level host board interface for RMCS Board Pro.
 *
 * This class owns the transport and protocol stack for a single board connection.
 * The supplied `Callback` is stored by reference, is not owned by the board, and must outlive the
 * board instance.
 *
 * The board may start transport I/O during construction, so receive callbacks may be invoked before
 * the board constructor returns.
 *
 * A common usage pattern is for an enclosing user type to inherit `Callback` and declare the board
 * as its last data member. In that arrangement, early callbacks may access base subobjects and
 * members whose initialization has already completed before the board member begins construction.
 *
 * @warning Early callbacks must not depend on invariants established later in an enclosing
 * constructor body, on post-construction configuration, or on the board object itself having
 * finished construction. Delay board construction with `std::optional` or `std::unique_ptr` when
 * callback behavior depends on such state.
 */
class RmcsBoardPro final {
public:
    class Callback : public data::DataCallback {
    public:
        // Channel descriptors for this board. Addressing a channel through its
        // descriptor is what lets generic code reach data_id and, for UARTs,
        // config_data_id without a second lookup table.
        struct Spec {
            using Can = spec::rmcs_board_pro::CanDescriptor;
            static constexpr spec::rmcs_board_pro::internal::CanDescriptors kCans{};

            using Uart = spec::rmcs_board_pro::UartDescriptor;
            static constexpr spec::rmcs_board_pro::internal::UartDescriptors kUarts{};

            using Gpio = spec::rmcs_board_pro::GpioDescriptor;
            static constexpr spec::rmcs_board_pro::internal::GpioDescriptors kGpios{};
        };

        struct View {
            using Can = data::CanDataView;
            using Uart = data::UartDataView;
            using UartConfig = data::UartConfigView;
            using GpioDigital = data::GpioDigitalDataView;
            using GpioAnalog = data::GpioAnalogDataView;
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

        virtual void dbus_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart0_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart1_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart2_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart3_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }

        virtual void gpio_digital_read_result_callback(
            const librmcs::spec::rmcs_board_pro::GpioDescriptor& gpio,
            const librmcs::data::GpioDigitalDataView& data) {
            (void)gpio;
            (void)data;
        }
        virtual void gpio_analog_read_result_callback(
            const librmcs::spec::rmcs_board_pro::GpioDescriptor& gpio,
            const librmcs::data::GpioAnalogDataView& data) {
            (void)gpio;
            (void)data;
        }

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
            case data::DataId::kUartDbus: dbus_receive_callback(data); return true;
            case data::DataId::kUart0: uart0_receive_callback(data); return true;
            case data::DataId::kUart1: uart1_receive_callback(data); return true;
            case data::DataId::kUart2: uart2_receive_callback(data); return true;
            case data::DataId::kUart3: uart3_receive_callback(data); return true;
            default: return false;
            }
        }

        bool gpio_digital_read_result_callback(
            uint8_t channel_index, const data::GpioDigitalDataView& data) final {
            if (channel_index >= spec::rmcs_board_pro::kGpioDescriptors.size()) [[unlikely]]
                return false;
            gpio_digital_read_result_callback(
                spec::rmcs_board_pro::kGpioDescriptors[channel_index], data);
            return true;
        }

        bool gpio_analog_read_result_callback(
            uint8_t channel_index, const data::GpioAnalogDataView& data) final {
            if (channel_index >= spec::rmcs_board_pro::kGpioDescriptors.size()) [[unlikely]]
                return false;
            gpio_analog_read_result_callback(
                spec::rmcs_board_pro::kGpioDescriptors[channel_index], data);
            return true;
        }
    };

    // Optional channel configuration, applied over EP0 and read back from the
    // hardware before this constructor returns. Leaving a field unset keeps
    // whatever the firmware brought that channel up with. A rejected baudrate,
    // or a CAN bus whose frame type is not the one asked for, throws here --
    // which is the whole reason configuration moved off the data stream, where
    // neither could be reported. See librmcs/board/rmcs_config.hpp.
    using Configuration = rmcs::Configuration;

    explicit RmcsBoardPro(
        Callback& callback = default_callback_, std::string_view serial_filter = {},
        const AdvancedOptions& options = {}, const Configuration& configuration = {})
        : configuration_(configuration)
        , handler_(
              0xA11C, 0xAF01, serial_filter, options, callback,
              [this](host::protocol::Handler& handler) {
                  interface_ = rmcs::apply(handler, configuration_);
              }) {}

    RmcsBoardPro(const RmcsBoardPro&) = delete;
    RmcsBoardPro& operator=(const RmcsBoardPro&) = delete;
    RmcsBoardPro(RmcsBoardPro&&) = delete;
    RmcsBoardPro& operator=(RmcsBoardPro&&) = delete;
    ~RmcsBoardPro() = default;

    class PacketBuilder {
        friend class RmcsBoardPro;

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
                    "RmcsBoardPro: CAN port out of range (CAN1..CAN4)"};
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

        PacketBuilder& uart1_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart1, data)) [[unlikely]]
                throw std::invalid_argument{"UART1 transmission failed: Invalid UART data"};
            return *this;
        }

        PacketBuilder& uart2_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart2, data)) [[unlikely]]
                throw std::invalid_argument{"UART2 transmission failed: Invalid UART data"};
            return *this;
        }

        PacketBuilder& uart3_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart3, data)) [[unlikely]]
                throw std::invalid_argument{"UART3 transmission failed: Invalid UART data"};
            return *this;
        }

        PacketBuilder& gpio_digital_write(
            const librmcs::spec::rmcs_board_pro::GpioDescriptor& gpio,
            const librmcs::data::GpioDigitalDataView& data) {
            if (!gpio.supports(spec::GpioCapability::kDigitalWrite)
                || !builder_.write_gpio_digital_data(gpio.channel_index, data)) [[unlikely]]
                throw std::invalid_argument{"GPIO digital transmission failed: Invalid GPIO data"};
            return *this;
        }
        PacketBuilder& gpio_digital_read(
            const librmcs::spec::rmcs_board_pro::GpioDescriptor& gpio,
            const librmcs::data::GpioReadConfigView& data) {
            if (!data.supported(gpio)
                || !builder_.write_gpio_digital_read_config(gpio.channel_index, data)) [[unlikely]]
                throw std::invalid_argument{
                    "GPIO digital read configuration transmission failed: Invalid GPIO data"};
            return *this;
        }
        PacketBuilder& gpio_analog_write(
            const librmcs::spec::rmcs_board_pro::GpioDescriptor& gpio,
            const librmcs::data::GpioAnalogDataView& data) {
            if (!gpio.supports(spec::GpioCapability::kAnalogWrite)
                || !builder_.write_gpio_analog_data(gpio.channel_index, data)) [[unlikely]]
                throw std::invalid_argument{"GPIO analog transmission failed: Invalid GPIO data"};
            return *this;
        }

    private:
        explicit PacketBuilder(host::protocol::Handler& handler) noexcept
            : builder_(handler.start_transmit()) {}

        host::protocol::Handler::PacketBuilder builder_;
    };
    // Whether this board's link is up, re-establishing, or gone for good.
    // kFaulted means the device disappeared: the transport refuses traffic and
    // only destroying this object and constructing a new one recovers it.
    [[nodiscard]] host::protocol::Handler::LinkState link_state() const noexcept {
        return handler_.link_state();
    }

    PacketBuilder start_transmit() noexcept { return PacketBuilder{handler_}; }

    // Runtime reconfiguration, over EP0 rather than in the data stream. Unlike
    // the retired in-band config field these are synchronous and verified: the
    // call returns only once the board has confirmed the new setting, and
    // throws if it refused. They are NOT ordered against queued data -- bytes
    // already handed to the board go out at whichever rate the port reaches
    // them at, so quiesce the link before switching.
    void configure_uart0(uint32_t baudrate) { rmcs::configure_uart(handler_, 0, baudrate); }

    // What UART0 is really running, reconstructed on the board from the
    // divisor actually programmed -- not the value that was last requested.
    uint32_t uart0_baudrate() { return rmcs::read_uart_baudrate(handler_, 0); }

    void configure_uart1(uint32_t baudrate) { rmcs::configure_uart(handler_, 1, baudrate); }

    // What UART1 is really running, reconstructed on the board from the
    // divisor actually programmed -- not the value that was last requested.
    uint32_t uart1_baudrate() { return rmcs::read_uart_baudrate(handler_, 1); }

    void configure_uart2(uint32_t baudrate) { rmcs::configure_uart(handler_, 2, baudrate); }

    // What UART2 is really running, reconstructed on the board from the
    // divisor actually programmed -- not the value that was last requested.
    uint32_t uart2_baudrate() { return rmcs::read_uart_baudrate(handler_, 2); }

    void configure_uart3(uint32_t baudrate) { rmcs::configure_uart(handler_, 3, baudrate); }

    // What UART3 is really running, reconstructed on the board from the
    // divisor actually programmed -- not the value that was last requested.
    uint32_t uart3_baudrate() { return rmcs::read_uart_baudrate(handler_, 3); }

    // Frame type of each CAN bus, as the board reported it during construction.
    // It is a property of the bus, not of a frame: CanDataView::is_fdcan is
    // ignored by this board's firmware, which sends every frame in its bus's
    // mode. Read this instead of assuming.
    [[nodiscard]] bool can1_is_fd() const { return interface_.can_fd(0); }

    [[nodiscard]] bool can2_is_fd() const { return interface_.can_fd(1); }

    [[nodiscard]] bool can3_is_fd() const { return interface_.can_fd(2); }

    [[nodiscard]] bool can4_is_fd() const { return interface_.can_fd(3); }

    // The controller's own error registers for one CAN port. Read over EP0 on
    // the shipping image; see librmcs/board/rmcs_config.hpp for how to read it.
    [[nodiscard]] rmcs::vc::CanStatusPayload can_status(rmcs::CanPort port) {
        return rmcs::read_can_status(handler_, static_cast<std::size_t>(port) - 1);
    }

    // How much of a CAN round trip happens on the board. Cycles; divide by
    // cpu_hz. See librmcs/board/rmcs_config.hpp.
    [[nodiscard]] rmcs::vc::LatencyBreakdownPayload latency_breakdown(bool reset = false) {
        return rmcs::read_latency_breakdown(handler_, reset);
    }

    // Channels the board reports it has. On a board directory that serves more
    // than one PCB (the hpm5321 image serves both the single- and dual-CAN
    // variants) this is the run-time truth, not the image's capacity.
    [[nodiscard]] const rmcs::Interface& interface() const { return interface_; }

private:
    static inline Callback default_callback_{};
    // Both declared BEFORE handler_ on purpose. The before-session hook runs
    // while handler_ is still being constructed and touches both, so their
    // lifetimes must already have begun -- member initialisation runs in
    // declaration order. configuration_ is a COPY: the hook runs again on every
    // reconnect, long after the constructor argument has gone.
    rmcs::Interface interface_{};
    Configuration configuration_;
    host::protocol::Handler handler_;
};

} // namespace librmcs::board
