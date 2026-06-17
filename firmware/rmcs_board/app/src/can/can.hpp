#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>

#include <hpm_common.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_regs.h>
#include <hpm_mcan_soc.h>
#include <hpm_soc.h>
#include <hpm_soc_feature.h>

#include "board_app.hpp"
#include "core/include/librmcs/data/datas.hpp"
#include "firmware/rmcs_board/app/src/can/can_port.hpp"
#include "core/src/protocol/protocol.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/rmcs_board/app/src/led/led.hpp"
#include "firmware/rmcs_board/app/src/usb/helper.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::can {

using board::CanMode;
using board::CanPort;

class Can : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Can, data::DataId, CanPort, uint32_t (*const)[], uint32_t>;

    // The two supported configurations are fixed: classic CAN 2.0 at 1Mbps, or
    // CAN-FD with 1Mbps arbitration and 5Mbps data phase (BRS on).
    static constexpr uint32_t kArbitrationBaudrate = 1'000'000;
    static constexpr uint32_t kCanFdDataBaudrate = 5'000'000;

    explicit Can(
        data::DataId data_id, CanPort port, uint32_t (*const ram_base)[], uint32_t ram_size)
        : data_id_(data_id)
        , can_base_(reinterpret_cast<MCAN_Type*>(port.base))
        , canfd_(port.mode == CanMode::kCanFd) {

        const mcan_msg_buf_attr_t attr = {
            .ram_base = reinterpret_cast<uintptr_t>(ram_base),
            .ram_size = ram_size,
        };
        auto status = mcan_set_msg_buf_attr(can_base_, &attr);
        core::utility::assert_always(status == status_success);

        const uint32_t can_source_clock_freq = board::init_can(can_base_);

        mcan_config_t config;
        mcan_get_default_config(can_base_, &config);
        config.baudrate = kArbitrationBaudrate;
        config.mode = mcan_mode_normal;
        config.enable_canfd = canfd_;
        if (canfd_)
            config.baudrate_fd = kCanFdDataBaudrate;
        // Keep the default 8-byte element size even for CAN-FD: the frames on this
        // bus never exceed 8 data bytes, so RAM usage stays identical to classic CAN.
        config.ram_config.txbuf_dedicated_txbuf_elem_count = 0;
        config.ram_config.txbuf_fifo_or_queue_elem_count = MCAN_TXBUF_SIZE_CAN_DEFAULT;
        config.ram_config.txfifo_or_txqueue_mode = MCAN_TXBUF_OPERATION_MODE_FIFO;
        config.disable_auto_retransmission = true;

        // Enable internal 16-bit hardware timestamp counter.
        // Clocked by CAN bit time: 1 Mbps → 1 us/tick.
        // Extended to 32 bits in handle_uplink() via delta accumulation.
        config.timestamp_cfg.counter_prescaler = 1;
        config.timestamp_cfg.timestamp_selection = MCAN_TIMESTAMP_SEL_VALUE_INCREMENT;

        mcan_init(can_base_, &config, can_source_clock_freq);
        mcan_enable_interrupts(can_base_, MCAN_INT_RXFIFO0_NEW_MSG);
        // CAN RX is the forwarding-critical path (motor feedback -> host), so it
        // takes a higher PLIC priority than the secondary UART (priority 1).
        // Higher number == more urgent on the PLIC; USB matches this at 2.
        intc_m_enable_irq_with_priority(port.irq_num, 2);
    }

    [[nodiscard]] data::DataId data_id() const { return data_id_; }

    void handle_downlink(const data::CanDataView& data) {
        mcan_tx_frame_t frame{};
        if (data.is_extended_can_id) {
            frame.use_ext_id = true;
            frame.ext_id = data.can_id;
        } else {
            frame.use_ext_id = false;
            frame.std_id = data.can_id;
        }
        frame.canfd_frame = canfd_;
        frame.bitrate_switch = canfd_; // CAN-FD frames switch to the data-phase baudrate
        frame.rtr = data.is_remote_transmission;

        core::utility::assert_debug(data.can_data.size() <= 8);
        frame.dlc = data.can_data.size();
        if (!data.can_data.empty())
            std::memcpy(frame.data_8, data.can_data.data(), data.can_data.size());

        const hpm_stat_t status = mcan_transmit_via_txfifo_nonblocking(can_base_, &frame, nullptr);
        if (status != status_success)
            led::led->downlink_buffer_full();
    }

    void handle_uplink(core::protocol::FieldId field_id, core::protocol::Serializer& serializer) {
        mcan_rx_message_t rx;
        core::utility::assert_always(mcan_read_rxfifo(can_base_, 0, &rx) == status_success);

        data::CanDataView data;
        const size_t data_length = rx.dlc;
        data.is_fdcan = false;
        data.is_extended_can_id = rx.use_ext_id;
        data.is_remote_transmission = rx.rtr;
        data.can_id = data.is_extended_can_id ? rx.ext_id : rx.std_id;
        data.can_data = {reinterpret_cast<const std::byte*>(rx.data_8), data_length};

        // Extract hardware timestamp (32-bit TSU or 16-bit internal, extended to 32 bits).
        mcan_timestamp_value_t ts_value;
        if (mcan_get_timestamp_from_received_message(can_base_, &rx, &ts_value)
            == status_success) {
            if (ts_value.is_32bit) {
                data.timestamp_us = ts_value.ts_32bit;
            } else if (ts_value.is_16bit) {
                // 16-bit internal timestamp at 1 us/tick → extend to 32 bits.
                // CAN frames arrive at ≥ 1 kHz, so the gap between frames is far less
                // than the 65 ms wraparound period — simple delta accumulation suffices.
                static uint32_t extended_us = 0;
                static uint16_t last_raw = 0;
                const uint16_t raw = ts_value.ts_16bit;
                extended_us += static_cast<uint16_t>(raw - last_raw);
                last_raw = raw;
                data.timestamp_us = extended_us;
            }
        }

        core::utility::assert_always(
            serializer.write_can(field_id, data)
            != core::protocol::Serializer::SerializeResult::kInvalidArgument);
    }

    void irq_handler() {
        const uint32_t flags = mcan_get_interrupt_flags(can_base_);

        if (!flags) [[unlikely]]
            return;

        if (flags & MCAN_INT_RXFIFO0_NEW_MSG) [[likely]]
            handle_uplink(data_id_, usb::get_serializer());

        mcan_clear_interrupt_flags(can_base_, flags);
    }

private:
    const data::DataId data_id_;
    MCAN_Type* can_base_;
    const bool canfd_;
};

// Everything below is built from the board's CAN port table (board::kCanPorts),
// so there are no per-port macros: the count, the FD mode and the dispatch all
// follow the table.
constexpr size_t kCanCount = std::size(board::kCanPorts);
static_assert(kCanCount >= 1 && kCanCount <= 4);

constexpr data::DataId kCanDataIds[] = {
    data::DataId::kCan0, data::DataId::kCan1, data::DataId::kCan2, data::DataId::kCan3};

ATTR_PLACE_AT(".ahb_sram")
inline constinit uint32_t can_msg_buffer[kCanCount][MCAN_MSG_BUF_SIZE_IN_WORDS]{};
static_assert(MCAN_SOC_MSG_BUF_IN_AHB_RAM == 1);

namespace internal {

template <std::size_t I>
consteval Can::Lazy make_can() {
    return Can::Lazy{
        kCanDataIds[I], board::kCanPorts[I], &can_msg_buffer[I], sizeof(can_msg_buffer[I])};
}

template <std::size_t... I>
consteval std::array<Can::Lazy, sizeof...(I)> make_can_array(std::index_sequence<I...>) {
    return {make_can<I>()...};
}

} // namespace internal

inline constinit auto can_array =
    internal::make_can_array(std::make_index_sequence<kCanCount>{});

} // namespace librmcs::firmware::can
