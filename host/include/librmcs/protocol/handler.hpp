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

        bool write_uart_config(data::DataId field_id, const data::UartConfigView& view) noexcept;

        bool write_gpio_digital_data(
            uint8_t channel_index, const data::GpioDigitalDataView& view) noexcept;

        bool write_gpio_digital_read_config(
            uint8_t channel_index, const data::GpioReadConfigView& view) noexcept;

        bool write_gpio_analog_data(
            uint8_t channel_index, const data::GpioAnalogDataView& view) noexcept;

    private:
        friend class Handler;

        explicit PacketBuilder(void* transport, bool cyclic) noexcept;

        // Two StreamBuffer + Serializer pairs since CAN can be routed to its own
        // transport channel. A static_assert in handler.cpp pins the real size.
        alignas(std::uintptr_t) std::uint8_t storage_[16 * sizeof(std::uintptr_t)];
    };

    Handler(
        uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
        const board::AdvancedOptions& options, data::DataCallback& callback);

    /**
     * @brief Connects to a board over EtherCAT (the rmcs_board EtherCAT stream
     * bridge) instead of USB.
     *
     * The backend is chosen at run time via the RMCS_ECAT_BACKEND environment
     * variable ("soem" or "igh"; default: igh if compiled in, else soem),
     * among the backends the SDK was built with (-DLIBRMCS_ENABLE_SOEM=ON /
     * -DLIBRMCS_ENABLE_IGH=ON).
     *
     * @param ethercat_interface_name For SOEM: the raw network interface wired
     *        to the slave (e.g. "enp2s0"); opening it requires CAP_NET_RAW (or
     *        root). For IgH: advisory only -- the NIC is owned by the IgH
     *        master kernel module (`ethercatctl start` first; the process
     *        needs write access to /dev/EtherCAT0).
     * @throws std::runtime_error when the SDK was built without any EtherCAT
     *         backend, when RMCS_ECAT_BACKEND names a backend that was not
     *         compiled in, or when the slave cannot be brought to OPERATIONAL.
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

    /**
     * @brief Starts one explicit latest-wins cyclic CAN batch.
     *
     * With the IgH hybrid PDO mode this is a strict fixed-PDO batch: it accepts
     * at most seven standard, non-RTR, <=8-byte CAN frames per bus. An excess,
     * unsupported, or non-CAN field returns false and invalidates the complete
     * snapshot; it never spills into the reliable stream. Send configuration
     * and event traffic through a separate start_transmit() builder. Other
     * transports treat this exactly like start_transmit(). A caller must create
     * at most one cyclic builder per control tick; a newer unsampled batch is
     * intentionally allowed to replace an older one. This removes transport
     * queue jitter, but an asynchronous caller still has 0..Tpdo sampling phase;
     * use a phase-locked cycle for hard control-loop jitter measurements.
     */
    PacketBuilder start_cyclic_transmit() noexcept;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

} // namespace librmcs::host::protocol
