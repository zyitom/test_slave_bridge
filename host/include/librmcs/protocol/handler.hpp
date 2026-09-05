#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

    /**
     * @brief Runs once with the transport up but no session open yet.
     *
     * This is the only window in which a transport-level handshake can precede
     * the first kStart. The rmcs_board classes use it to apply and verify their
     * EP0 channel configuration, so that a session never opens against a board
     * whose configuration the host has not confirmed.
     *
     * Throwing from here aborts construction, exactly as a failed session would.
     */
    using BeforeSession = std::function<void(Handler&)>;

    Handler(
        uint16_t usb_vid, std::span<const uint16_t> usb_pids, std::string_view serial_filter,
        const board::AdvancedOptions& options, data::DataCallback& callback,
        const BeforeSession& before_session = nullptr);

    /**
     * @brief Single-product convenience overload.
     *
     * The span above exists because one firmware image can ship under several
     * product IDs; a board with exactly one says so here instead of declaring a
     * one-element array. The value is used during construction only, so the
     * temporary it spans cannot outlive its use.
     */
    Handler(
        uint16_t usb_vid, uint16_t usb_pid, std::string_view serial_filter,
        const board::AdvancedOptions& options, data::DataCallback& callback,
        const BeforeSession& before_session = nullptr)
        : Handler(
              usb_vid, std::span<const uint16_t>{&usb_pid, 1}, serial_filter, options, callback,
              before_session) {}

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

    /**
     * @brief What the link is doing, for an application that has to react.
     *
     * The keepalive thread repairs what it can on its own, so kSessionDown is
     * normal and transient -- it is what a board looks like for the few hundred
     * milliseconds after a hiccup. kFaulted is terminal for THIS object: the
     * transport has stopped accepting traffic and only destroying the board and
     * constructing a new one can recover it.
     */
    enum class LinkState : std::uint8_t {
        kUp,          // session established; data is flowing
        kSessionDown, // transport alive, session being (re)established
        kFaulted,     // the device is gone; this board object cannot recover
    };

    [[nodiscard]] LinkState link_state() const noexcept;

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

    /**
     * @brief Asks the board to fire a hardware synchronisation pulse at an
     * absolute microframe of the shared USB-SOF timeline.
     *
     * Send the IDENTICAL microframe to every board in one round: each board's
     * capture offset from it carries the path delay plus the skew with opposite
     * sign, so differencing the boards' reports cancels the path delay. Requires
     * firmware built with -DLIBRMCS_PULSE_TEST=ON.
     */
    void send_pulse_schedule(uint64_t microframe) noexcept;

    /**
     * @brief Largest EP0 configuration payload this API will carry.
     *
     * Every payload in librmcs/protocol/vendor_control.hpp fits well inside
     * this; the bound exists so the implementation can stage the transfer in a
     * fixed buffer instead of allocating per request.
     */
    static constexpr size_t kVendorControlPayloadMax = 64;

    /**
     * @brief Sends one EP0 vendor configuration request to the board.
     *
     * The channel configuration path: UART baudrates and CAN bus modes travel
     * here rather than in the data stream, because a control transfer's status
     * stage reports whether the board accepted the setting and the bulk stream
     * cannot. Board classes call this while constructing, and read the setting
     * back before returning, so a constructed board object is a board whose
     * configuration is known rather than assumed.
     *
     * Independent of the session handshake: no nonce, no keepalive, and it
     * works before the first session has opened.
     *
     * @param request   A core::protocol::vendor_control::Request code.
     * @param index     Channel index (CAN bus or UART port); wIndex on the wire.
     * @param payload   Bytes to send (out) or the buffer to fill (in).
     * @param size      Payload size; must match what the board expects exactly.
     *
     * @return true when the board completed the request. false means the board
     *         STALLed it -- an explicit rejection, with nothing changed on the
     *         board; for a baudrate that is the divisor solver refusing the rate.
     *
     * @throws std::runtime_error when the transport has no control endpoint
     *         (any EtherCAT backend) or the transfer failed outright.
     * @throws std::invalid_argument when size exceeds kVendorControlPayloadMax.
     */
    bool vendor_control_out(uint8_t request, uint16_t index, const void* payload, size_t size);
    bool vendor_control_in(uint8_t request, uint16_t index, void* payload, size_t size);

private:
    class Impl;
    Impl* impl_ = nullptr;
};

} // namespace librmcs::host::protocol
