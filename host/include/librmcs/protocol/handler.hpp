#pragma once

#include <cstdint>
#include <string_view>

#include <librmcs/board/common.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/export.hpp>

namespace librmcs::host::protocol {

class LIBRMCS_API Handler {
public:
    class LIBRMCS_API PacketBuilder {
    public:
        PacketBuilder(const PacketBuilder&) = delete;
        PacketBuilder& operator=(const PacketBuilder&) = delete;
        PacketBuilder(PacketBuilder&&) = delete;
        PacketBuilder& operator=(PacketBuilder&&) = delete;

        ~PacketBuilder() noexcept;

        bool write_can(data::DataId field_id, const data::CanDataView& view) noexcept;

        bool write_uart(data::DataId field_id, const data::UartDataView& view) noexcept;

        bool write_gpio_digital_data(
            uint8_t channel_index, const data::GpioDigitalDataView& view) noexcept;

        bool write_gpio_digital_read_config(
            uint8_t channel_index, const data::GpioReadConfigView& view) noexcept;

        bool write_gpio_analog_data(
            uint8_t channel_index, const data::GpioAnalogDataView& view) noexcept;

    private:
        friend class Handler;

        explicit PacketBuilder(void* transport) noexcept;

        alignas(std::uintptr_t) std::uint8_t storage_[6 * sizeof(std::uintptr_t)];
    };

    Handler(
        uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
        const board::AdvancedOptions& options, data::DataCallback& callback);

    /**
     * @brief Connects to a board over EtherCAT (the rmcs_board EtherCAT stream
     * bridge) instead of USB.
     *
     * @param ethercat_interface_name Raw network interface wired to the slave
     *        (e.g. "enp2s0"); opening it requires CAP_NET_RAW (or root).
     * @throws std::runtime_error when the SDK was built without the optional
     *         SOEM component (cmake -DLIBRMCS_ENABLE_SOEM=ON), or when the
     *         slave cannot be brought to OPERATIONAL.
     *
     * The session handshake and all data semantics are identical to the USB
     * transport; use options.thread_setup to pin the EtherCAT busy-poll
     * thread to an isolated core for the lowest latency.
     */
    Handler(
        std::string_view ethercat_interface_name, const board::AdvancedOptions& options,
        data::DataCallback& callback);

    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&& other) noexcept;
    Handler& operator=(Handler&& other) noexcept;

    ~Handler() noexcept;

    PacketBuilder start_transmit() noexcept;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

} // namespace librmcs::host::protocol
