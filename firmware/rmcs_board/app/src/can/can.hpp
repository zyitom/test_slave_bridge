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
#include "core/src/protocol/protocol.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/rmcs_board/app/src/can/can_port.hpp"
#include "firmware/rmcs_board/app/src/led/led.hpp"
#include "firmware/rmcs_board/app/src/link/uplink.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::can {

using board::CanMode;
using board::CanPort;

class Can : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Can, data::DataId, CanPort, size_t>;

    // Baudrates are compile-time constants; the mode is fixed per board via
    // the CanPort table in board_app.hpp.
    static constexpr uint32_t kArbitrationBaudrate = 1'000'000;
    static constexpr uint32_t kCanFdDataBaudrate = 5'000'000;

    explicit Can(data::DataId data_id, CanPort port, size_t board_can_index)
        : data_id_(data_id)
        , can_base_(reinterpret_cast<MCAN_Type*>(port.base))
        , canfd_(port.mode == CanMode::kCanFd) {

        // Message RAM placement is board business: SoCs differ in where MCAN
        // buffers may live (dedicated AHB RAM vs a section-placed array), so
        // the board hands out the region (board_app.cpp).
        const mcan_msg_buf_attr_t attr = board::can_message_ram(board_can_index);
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
        config.tsu_config.prescaler = 1; // unused for an external timebase
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
        // (160 * 6 = 960 at 160 MHz). handle_uplink() bakes this divisor in as
        // the compile-time kTsNsPerUs so the ISR needs no runtime division --
        // verify the clock tree still matches it.
        const uint32_t ptpc_step_ns = 1'000'000'000U / ptpc_freq;
        core::utility::assert_always((ptpc_freq / 1'000'000U) * ptpc_step_ns == kTsNsPerUs);

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
        mcan_enable_interrupts(
            can_base_,
#if defined(RMCS_ECAT_NATIVE_CAN) && RMCS_ECAT_NATIVE_CAN
            // Native variant polls RX FIFO0 from the core1 loop (read_native),
            // so the RX-new-message interrupt is left disabled -- its ISR path
            // reaches host_link, which the native build does not construct.
            MCAN_INT_BUS_OFF_STATUS | MCAN_INT_WARNING_STATUS | MCAN_INT_ERROR_PASSIVE
                | MCAN_INT_PROTOCOL_ERR_IN_ARB_PHASE | MCAN_INT_PROTOCOL_ERR_IN_DATA_PHASE);
#else
            MCAN_INT_RXFIFO0_NEW_MSG | MCAN_INT_BUS_OFF_STATUS | MCAN_INT_WARNING_STATUS
                | MCAN_INT_ERROR_PASSIVE | MCAN_INT_PROTOCOL_ERR_IN_ARB_PHASE
                | MCAN_INT_PROTOCOL_ERR_IN_DATA_PHASE);
#endif
        // CAN RX is the forwarding-critical path (motor feedback -> host).
        // Priority 3: above USB (2) and UART (1) — ensures CAN frames are
        // never delayed by bulk USB transfers or DMA callbacks.
        intc_m_enable_irq_with_priority(port.irq_num, 3);
    }

    [[nodiscard]] data::DataId data_id() const { return data_id_; }

    // Forwarding hot path -- defined out-of-line in can.cpp, in the ILM (.fast)
    // section, to remove FLASH-XIP fetch jitter from the worst case. See can.cpp
    // for the rationale and why they are not inline-in-class.
    // handle_uplink reads (at most) one frame from RX FIFO0 and returns whether
    // a frame was consumed, so the ISR can drain the FIFO in a loop.
    void handle_downlink(const data::CanDataView& data);
    bool handle_uplink(core::protocol::FieldId field_id, core::protocol::Serializer& serializer);
    void irq_handler();

    // Native variant (RMCS_ECAT_NATIVE_CAN): read one frame straight out of RX
    // FIFO0 into `out`, copying its data bytes into caller-owned `storage[8]`.
    // Returns false when the FIFO is empty. Frames whose stored payload was
    // truncated by the 8-byte element size (FD DLC > 8) are dropped while the
    // drain continues, mirroring handle_uplink(). This bypasses the serializer
    // so the core1 poll loop can forward representable frames as raw mailbox
    // records; that loop explicitly drops extended and RTR frames.
    bool read_native(data::CanDataView& out, uint8_t storage[8]) {
        for (;;) {
            mcan_rx_message_t rx;
            if (mcan_read_rxfifo(can_base_, 0, &rx) != status_success)
                return false;
            if (rx.canfd_frame && rx.dlc > 8) [[unlikely]]
                continue; // drained but unforwardable; keep draining
            const size_t length = rx.rtr ? 0U : (rx.dlc > 8U ? 8U : rx.dlc);
            out.is_fdcan = rx.canfd_frame;
            out.is_extended_can_id = rx.use_ext_id;
            out.is_remote_transmission = rx.rtr;
            out.can_id = out.is_extended_can_id ? rx.ext_id : rx.std_id;
            if (length != 0)
                std::memcpy(storage, rx.data_8, length);
            out.can_data = {reinterpret_cast<const std::byte*>(storage), length};
            return true;
        }
    }

private:
    // Read and normalize one RX FIFO frame. `true` means an element was
    // consumed; `valid` distinguishes a representable frame from an FD payload
    // that the configured 8-byte RX element had to truncate.
    bool read_uplink(data::CanDataView& out, uint8_t storage[8], bool& valid);
    static void serialize_uplink(
        core::protocol::FieldId field_id, const data::CanDataView& data,
        core::protocol::Serializer& serializer);

    // Classifies an MCAN Last Error Code (arbitration or data phase) into an
    // indicator-LED state -- the granularity a CAN controller can actually back
    // up electrically:
    //   ack_error  -> kNoAck       : frame sent fine, nobody acknowledged.
    //   bit0_error -> kWiringFault : sent dominant, read back recessive -- the bus
    //                                cannot be driven dominant (CAN_H/L shorted,
    //                                reversed, or open).  Stable on a hard fault.
    //   stuff/form/crc/bit1 -> kSignalError : corrupted bits; the exact code
    //                                fluctuates and its causes (no termination,
    //                                baudrate mismatch, noise) are indistinguishable.
    //   no_error/no_change -> kNone.
    static led::CanFault classify_can_fault(uint8_t last_error_code) {
        switch (last_error_code) {
        case mcan_last_error_code_no_error:
        case mcan_last_error_code_no_change: return led::CanFault::kNone;
        case mcan_last_error_code_ack_error: return led::CanFault::kNoAck;
        case mcan_last_error_code_bit0_error: return led::CanFault::kWiringFault;
        default: return led::CanFault::kSignalError; // stuff / format / bit1 / crc
        }
    }

    // Divisor that turns a PTPC reported-nanosecond count into true
    // microseconds; fixed at compile time so the ISR-side conversion uses only
    // constant divisions (multiply-and-shift after GCC), and asserted against
    // the real clock tree in the constructor. The value depends on the board's
    // PTPC (AHB) clock -- e.g. 960 = 160 MHz * 6 ns step on HPM5321, 1000 =
    // 200 MHz * 5 ns step on HPM6E80 -- so each board_app.hpp provides it.
    static constexpr uint32_t kTsNsPerUs = board::kCanTimestampNsPerUs;

    const data::DataId data_id_;
    MCAN_Type* can_base_;
    const bool canfd_;
};

// Everything below is built from the board's CAN port table (board::kCanPorts),
// so there are no per-port macros: the count, the FD mode and the dispatch all
// follow the table. Message RAM comes from board::can_message_ram().
constexpr size_t kCanCount = std::size(board::kCanPorts);
static_assert(kCanCount >= 1 && kCanCount <= 4);

constexpr data::DataId kCanDataIds[] = {
    data::DataId::kCan0, data::DataId::kCan1, data::DataId::kCan2, data::DataId::kCan3};

namespace internal {

template <std::size_t I>
consteval Can::Lazy make_can() {
    return Can::Lazy{kCanDataIds[I], board::kCanPorts[I], I};
}

template <std::size_t... I>
consteval std::array<Can::Lazy, sizeof...(I)> make_can_array(std::index_sequence<I...>) {
    return {make_can<I>()...};
}

} // namespace internal

inline constinit auto can_array = internal::make_can_array(std::make_index_sequence<kCanCount>{});

} // namespace librmcs::firmware::can
