#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>

#include <hpm_clock_drv.h>
#include <hpm_common.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_regs.h>
#include <hpm_mcan_soc.h>
#include <hpm_ptpc_drv.h>
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

    // Baudrates are compile-time constants; the mode is fixed per board via
    // the CanPort table in board_app.hpp.
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

        // 64-bit hardware timestamp via the Timestamp Unit (TSU). This SoC's
        // MCAN has no usable internal TSU timebase (TBCS is fixed to "external"
        // by synthesis), so every controller's TSU is fed from a single shared
        // PTPC0 timebase routed through TBSEL slot 0. PTPC keeps an IEEE-1588
        // {seconds:nanoseconds} counter; handle_uplink() folds it to
        // microseconds. Because all controllers share PTPC0, their timestamps
        // sit on one common clock and are directly comparable across buses.
        config.use_timestamping_unit = true;
        config.tsu_config.enable_tsu = true;
        config.tsu_config.enable_64bit_timestamp = true;
        config.tsu_config.use_ext_timebase = true;
        config.tsu_config.ext_timebase_src = MCAN_TSU_EXT_TIMEBASE_SRC_TBSEL_0;
        config.tsu_config.tbsel_option = MCAN_TSU_TBSEL_PTPC0;
        config.tsu_config.capture_on_sof = true;
        config.tsu_config.prescaler = 1;  // unused for an external timebase
        config.timestamp_cfg.counter_prescaler = 1;
        config.timestamp_cfg.timestamp_selection = MCAN_TIMESTAMP_SEL_EXT_TS_VAL_USED;

        // The external TSU only timestamps a received frame when the filter that
        // accepts it is marked as a sync message (evaluated only while CCCR.UTSU
        // is set). The default accept-all filters have sync_message = 0, so swap
        // in accept-all (mask 0) sync filters for both standard and extended IDs
        // -- otherwise no frame is ever timestamped.
        mcan_filter_elem_t std_sync_filter{};
        std_sync_filter.filter_type = MCAN_FILTER_TYPE_CLASSIC_FILTER;
        std_sync_filter.filter_config = MCAN_FILTER_ELEM_CFG_STORE_IN_RX_FIFO0_IF_MATCH;
        std_sync_filter.can_id_type = MCAN_CAN_ID_TYPE_STANDARD;
        std_sync_filter.sync_message = 1U;
        std_sync_filter.filter_id = 0U;
        std_sync_filter.filter_mask = 0U;
        mcan_filter_elem_t ext_sync_filter = std_sync_filter;
        ext_sync_filter.can_id_type = MCAN_CAN_ID_TYPE_EXTENDED;
        config.all_filters_config.std_id_filter_list.filter_elem_list = &std_sync_filter;
        config.all_filters_config.std_id_filter_list.mcan_filter_elem_count = 1;
        config.all_filters_config.ext_id_filter_list.filter_elem_list = &ext_sync_filter;
        config.all_filters_config.ext_id_filter_list.mcan_filter_elem_count = 1;

        // Start the shared PTPC0 timebase once, then point this controller's TSU
        // input at it. PTPC is clocked from the AHB clock group (clock_ptpc).
        const uint32_t ptpc_freq = clock_get_frequency(clock_ptpc);
        // PTPC digital mode advances its nanosecond counter by an integer step
        // of floor(1e9 / ptpc_freq) ns (6 ns at 160 MHz) rather than the exact
        // 1e9 / ptpc_freq (6.25 ns), so reported time runs slow. Converting
        // reported nanoseconds to microseconds by dividing by
        // (ptpc_freq_MHz * step) instead of 1000 cancels the error exactly
        // (160 * 6 = 960 at 160 MHz). See handle_uplink().
        const uint32_t ptpc_step_ns = 1'000'000'000U / ptpc_freq;
        ts_ns_per_us_ = (ptpc_freq / 1'000'000U) * ptpc_step_ns;

        static bool ptpc_timebase_started = false;
        if (!ptpc_timebase_started) {
            ptpc_config_t ptpc_config;
            ptpc_get_default_config(HPM_PTPC, &ptpc_config);
            ptpc_config.src_frequency = ptpc_freq;
            ptpc_config.ns_rollover_mode = ptpc_ns_counter_rollover_digital;
            core::utility::assert_always(
                ptpc_init(HPM_PTPC, PTPC_PTPC_0, &ptpc_config) == status_success);
            ptpc_init_timer(HPM_PTPC, PTPC_PTPC_0);
            ptpc_timebase_started = true;
        }
        ptpc_set_timer_output(HPM_PTPC, mcan_get_instance_from_base(can_base_), false);

        mcan_init(can_base_, &config, can_source_clock_freq);
        mcan_enable_interrupts(can_base_, MCAN_INT_RXFIFO0_NEW_MSG);
        // CAN RX is the forwarding-critical path (motor feedback -> host).
        // Priority 3: above USB (2) and UART (1) — ensures CAN frames are
        // never delayed by bulk USB transfers or DMA callbacks.
        intc_m_enable_irq_with_priority(port.irq_num, 3);
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
        frame.bitrate_switch = canfd_;
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
        data.is_fdcan = rx.canfd_frame;
        data.is_extended_can_id = rx.use_ext_id;
        data.is_remote_transmission = rx.rtr;
        data.can_id = data.is_extended_can_id ? rx.ext_id : rx.std_id;
        data.can_data = {reinterpret_cast<const std::byte*>(rx.data_8), data_length};

        // 64-bit TSU timestamp captured at SOF from the shared PTPC0 timebase.
        // PTPC delivers an IEEE-1588 {seconds:nanoseconds} pair (high 32 bits =
        // seconds, low 32 bits = nanoseconds in [0, 1e9)); recombine into a
        // single nanosecond count, then divide by ts_ns_per_us_ (= 960 at
        // 160 MHz, not 1000) to convert to microseconds and undo the PTPC
        // digital-step error in one step. The result is truncated to 32 bits for
        // the wire: it wraps every ~71.6 min, but the host only uses deltas,
        // which are wrap-safe. status_mcan_timestamp_not_exist (frame not matched
        // by a sync filter) leaves the field as std::nullopt.
        mcan_timestamp_value_t ts_value;
        if (mcan_get_timestamp_from_received_message(can_base_, &rx, &ts_value)
                == status_success
            && ts_value.is_64bit) {
            const uint64_t raw = ts_value.ts_64bit;
            const uint64_t reported_ns =
                static_cast<uint64_t>(static_cast<uint32_t>(raw >> 32)) * 1'000'000'000ULL
                + static_cast<uint32_t>(raw);
            data.timestamp_us = static_cast<uint32_t>(reported_ns / ts_ns_per_us_);
        }

        const auto result = serializer.write_can(field_id, data);
        if (result == core::protocol::Serializer::SerializeResult::kBadAlloc) [[unlikely]]
            led::led->uplink_buffer_full();
        core::utility::assert_always(
            result != core::protocol::Serializer::SerializeResult::kInvalidArgument);
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
    // Divisor that turns a PTPC reported-nanosecond count into true
    // microseconds (960 at 160 MHz); set from the PTPC clock in the constructor.
    uint32_t ts_ns_per_us_ = 1000;
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
