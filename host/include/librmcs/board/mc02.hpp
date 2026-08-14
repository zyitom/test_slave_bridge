#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <librmcs/board/common.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>
#include <librmcs/spec/gpio.hpp>
#include <librmcs/spec/mc02/can.hpp>
#include <librmcs/spec/mc02/gpio.hpp>
#include <librmcs/spec/mc02/uart.hpp>

namespace librmcs::board {

/**
 * @brief High-level host board interface for mc02.
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
class Mc02 final {
public:
    class Callback : public data::DataCallback {
    public:
        // Channel descriptors for this board. Addressing a channel through its
        // descriptor is what lets generic code reach data_id and, for UARTs,
        // config_data_id without a second lookup table.
        struct Spec {
            using Can = spec::mc02::CanDescriptor;
            static constexpr spec::mc02::internal::CanDescriptors kCans{};

            using Uart = spec::mc02::UartDescriptor;
            static constexpr spec::mc02::internal::UartDescriptors kUarts{};

            using Gpio = spec::mc02::GpioDescriptor;
            static constexpr spec::mc02::internal::GpioDescriptors kGpios{};
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

        virtual void can1_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }
        virtual void can2_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }
        virtual void can3_receive_callback(const librmcs::data::CanDataView& data) { (void)data; }

        virtual void dbus_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart1_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart2_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }
        virtual void uart3_receive_callback(const librmcs::data::UartDataView& data) { (void)data; }

        // RS-485 port on USART2, present only in a -DLIBRMCS_APP_RS485_ENABLE
        // firmware. It shares kUart0 with the diagnostic channel below because
        // that is the one channel id mc02 leaves free, which is also why
        // app/CMakeLists.txt makes RS485, CAN_DIAG and LOOP_PROFILE mutually
        // exclusive -- at most one of the two meanings is live in any build.
        // Defaulting to the diagnostic behaviour means an application that
        // overrides neither keeps working exactly as it did before this existed.
        virtual void rs485_1_receive_callback(const librmcs::data::UartDataView& data) {
            diagnostic_receive_callback(data);
        }
        // Second RS-485 port, on USART3. Unlike rs485_1 it owns kUart4 outright,
        // an id added to the protocol for it, so it collides with nothing and
        // ignores by default rather than falling through to the diagnostics.
        virtual void rs485_2_receive_callback(const librmcs::data::UartDataView& data) {
            (void)data;
        }
        // Telemetry from a -DLIBRMCS_APP_CAN_DIAG firmware, on the otherwise
        // unused kUart0 id. Ignored by default so ordinary applications are
        // unaffected by a diagnostic build.
        virtual void diagnostic_receive_callback(const librmcs::data::UartDataView& data) {
            (void)data;
        }

        virtual void gpio_digital_read_result_callback(
            const librmcs::spec::mc02::GpioDescriptor& gpio,
            const librmcs::data::GpioDigitalDataView& data) {
            (void)gpio;
            (void)data;
        }
        virtual void gpio_analog_read_result_callback(
            const librmcs::spec::mc02::GpioDescriptor& gpio,
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
        // mc02 firmware does not report IMU temperature; kept to satisfy the interface.
        void temperature_receive_callback(
            const librmcs::data::ImuTemperatureDataView& data) override {
            (void)data;
        }

    public:
        bool can_receive_callback(data::DataId id, const data::CanDataView& data) final {
            switch (id) {
            case data::DataId::kCan1: can1_receive_callback(data); return true;
            case data::DataId::kCan2: can2_receive_callback(data); return true;
            case data::DataId::kCan3: can3_receive_callback(data); return true;
            default: return false;
            }
        }

        bool uart_receive_callback(data::DataId id, const data::UartDataView& data) final {
            switch (id) {
            case data::DataId::kUartDbus: dbus_receive_callback(data); return true;
            case data::DataId::kUart1: uart1_receive_callback(data); return true;
            case data::DataId::kUart2: uart2_receive_callback(data); return true;
            case data::DataId::kUart3: uart3_receive_callback(data); return true;
            // kUart0 is not one of this board's ordinary UARTs (those are
            // kUart1..3 plus DBUS), so it carries whichever optional channel the
            // firmware was built with: the RS-485 port, or LIBRMCS_APP_CAN_DIAG /
            // LOOP_PROFILE telemetry, the same convention rmcs_board follows.
            // Accepting it here matters either way: returning false makes the
            // deserializer treat the frame as a protocol error and tear the
            // session down, so a diagnostic build would kill the link it is meant
            // to be diagnosing.
            case data::DataId::kUart0: rs485_1_receive_callback(data); return true;
            case data::DataId::kUart4: rs485_2_receive_callback(data); return true;
            default: return false;
            }
        }

        bool gpio_digital_read_result_callback(
            uint8_t channel_index, const data::GpioDigitalDataView& data) final {
            if (channel_index >= spec::mc02::kGpioDescriptors.size()) [[unlikely]]
                return false;
            gpio_digital_read_result_callback(spec::mc02::kGpioDescriptors[channel_index], data);
            return true;
        }

        bool gpio_analog_read_result_callback(
            uint8_t channel_index, const data::GpioAnalogDataView& data) final {
            if (channel_index >= spec::mc02::kGpioDescriptors.size()) [[unlikely]]
                return false;
            gpio_analog_read_result_callback(spec::mc02::kGpioDescriptors[channel_index], data);
            return true;
        }
    };

    // mc02 uses the shared RMCS vendor id (0xA11C) and the fixed board-type PID
    // 0xD402; per-device identity lives in the serial number, so pass a
    // serial_filter to target a specific board when several are connected.
    explicit Mc02(
        Callback& callback = default_callback_, std::string_view serial_filter = {},
        const AdvancedOptions& options = {})
        : handler_(0xA11C, 0xD402, serial_filter, options, callback) {}

    Mc02(const Mc02&) = delete;
    Mc02& operator=(const Mc02&) = delete;
    Mc02(Mc02&&) = delete;
    Mc02& operator=(Mc02&&) = delete;
    ~Mc02() = default;

    class PacketBuilder {
        friend class Mc02;

    public:
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

        PacketBuilder& uart1_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart1, data)) [[unlikely]]
                throw std::invalid_argument{"UART1 transmission failed: Invalid UART data"};
            return *this;
        }

        // Runtime reconfiguration of UART1. Rides the same downlink stream as
        // the data above, so it is ordered against it: bytes queued earlier in
        // this batch are sent at the old baudrate, later ones at the new one.
        PacketBuilder& uart1_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart1Config, config)) [[unlikely]]
                throw std::invalid_argument{"UART1 configuration failed: Invalid UART config"};
            return *this;
        }
        PacketBuilder& uart2_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart2, data)) [[unlikely]]
                throw std::invalid_argument{"UART2 transmission failed: Invalid UART data"};
            return *this;
        }

        // Runtime reconfiguration of UART2. Rides the same downlink stream as
        // the data above, so it is ordered against it: bytes queued earlier in
        // this batch are sent at the old baudrate, later ones at the new one.
        PacketBuilder& uart2_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart2Config, config)) [[unlikely]]
                throw std::invalid_argument{"UART2 configuration failed: Invalid UART config"};
            return *this;
        }
        PacketBuilder& uart3_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart3, data)) [[unlikely]]
                throw std::invalid_argument{"UART3 transmission failed: Invalid UART data"};
            return *this;
        }

        // Runtime reconfiguration of UART3. Rides the same downlink stream as
        // the data above, so it is ordered against it: bytes queued earlier in
        // this batch are sent at the old baudrate, later ones at the new one.
        PacketBuilder& uart3_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart3Config, config)) [[unlikely]]
                throw std::invalid_argument{"UART3 configuration failed: Invalid UART config"};
            return *this;
        }

        // RS-485 port on USART2, reachable only from a -DLIBRMCS_APP_RS485_ENABLE
        // firmware. Addressing it on any other build is not merely ignored: that
        // firmware's downlink switch has no kUart0 case, returns false, and the
        // deserializer reports a protocol error and drops the session. That is
        // also why kUart0 is deliberately absent from Spec::kUarts -- generic
        // code walking every descriptor would otherwise take a default firmware
        // offline. Reach this port through these two methods only.
        //
        // It is a byte pipe like the ports above; nothing here knows what device
        // is on the bus. Firmware owns the half-duplex turnaround alone -- one
        // queued packet per bus transaction, released once the peer answers or a
        // deadline expires -- so a caller need not pace its requests by hand.
        // Everything above that layer is the caller's: framing, checksums, node
        // addressing, retry policy and offline detection.
        PacketBuilder& rs485_1_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart0, data)) [[unlikely]]
                throw std::invalid_argument{"RS485-1 transmission failed: Invalid UART data"};
            return *this;
        }

        // Runtime reconfiguration of the RS-485 port. Rides the same downlink
        // stream as the data above, so it is ordered against it: bytes queued
        // earlier in this batch are sent at the old baudrate, later ones at the
        // new one.
        PacketBuilder& rs485_1_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart0Config, config)) [[unlikely]]
                throw std::invalid_argument{"RS485-1 configuration failed: Invalid UART config"};
            return *this;
        }

        // Second RS-485 port, on USART3. Same firmware build requirement as
        // rs485_1 above -- both come from -DLIBRMCS_APP_RS485_ENABLE, and a
        // firmware without it rejects kUart4 the same way, dropping the session.
        PacketBuilder& rs485_2_transmit(const librmcs::data::UartDataView& data) {
            if (!builder_.write_uart(data::DataId::kUart4, data)) [[unlikely]]
                throw std::invalid_argument{"RS485-2 transmission failed: Invalid UART data"};
            return *this;
        }

        PacketBuilder& rs485_2_config(const librmcs::data::UartConfigView& config) {
            if (!builder_.write_uart_config(data::DataId::kUart4Config, config)) [[unlikely]]
                throw std::invalid_argument{"RS485-2 configuration failed: Invalid UART config"};
            return *this;
        }

        // mc02 exposes four PWM-capable pins, each usable as digital output,
        // PWM/analog output, or digital input (read configuration below).
        PacketBuilder& gpio_digital_write(
            const librmcs::spec::mc02::GpioDescriptor& gpio,
            const librmcs::data::GpioDigitalDataView& data) {
            if (!gpio.supports(spec::GpioCapability::kDigitalWrite)
                || !builder_.write_gpio_digital_data(gpio.channel_index, data)) [[unlikely]]
                throw std::invalid_argument{"GPIO digital transmission failed: Invalid GPIO data"};
            return *this;
        }
        PacketBuilder& gpio_analog_write(
            const librmcs::spec::mc02::GpioDescriptor& gpio,
            const librmcs::data::GpioAnalogDataView& data) {
            if (!gpio.supports(spec::GpioCapability::kAnalogWrite)
                || !builder_.write_gpio_analog_data(gpio.channel_index, data)) [[unlikely]]
                throw std::invalid_argument{"GPIO analog transmission failed: Invalid GPIO data"};
            return *this;
        }
        PacketBuilder& gpio_digital_read(
            const librmcs::spec::mc02::GpioDescriptor& gpio,
            const librmcs::data::GpioReadConfigView& data) {
            if (!data.supported(gpio)
                || !builder_.write_gpio_digital_read_config(gpio.channel_index, data)) [[unlikely]]
                throw std::invalid_argument{
                    "GPIO digital read configuration transmission failed: Invalid GPIO data"};
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
