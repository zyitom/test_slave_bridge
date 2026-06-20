#include "firmware/rmcs_board/app/src/can/can.hpp"

#include <cstddef>
#include <cstring>

#include "core/src/utility/assert.hpp"

// The CAN forwarding hot path is defined out-of-line here, in the ILM (.fast)
// section, rather than inline in can.hpp. ILM is zero-wait-state and never
// I-cache-misses, so this removes FLASH-XIP fetch jitter from the worst-case
// forwarding latency. Out-of-line (not inline-in-class) is deliberate: an
// inline/COMDAT function placed in .fast collides with the plain .fast
// functions here (a GCC "section type conflict"). Leaf calls into the MCAN
// driver (mcan_read_rxfifo) and the shared serializer (write_can) stay in
// FLASH -- they are SDK/core code -- so the win is on our glue plus the inlined
// MCAN register helpers, not the whole path.

namespace librmcs::firmware::can {

ATTR_PLACE_AT(".fast")
void Can::handle_downlink(const data::CanDataView& data) {
    mcan_tx_frame_t frame{};
    if (data.is_extended_can_id) {
        frame.use_ext_id = true;
        frame.ext_id = data.can_id;
    } else {
        frame.use_ext_id = false;
        frame.std_id = data.can_id;
    }
    // The controller stays permanently in CAN-FD mode, which is a strict
    // superset: an FD-enabled M_CAN transmits and receives classic CAN 2.0
    // frames too, selected per element via the FDF/BRS bits.  So the frame
    // type is chosen per-frame from the host's is_fdcan flag instead of a
    // fixed controller mode -- no INIT-mode reconfiguration, no bus
    // interruption, just two bool assignments on the hot path.  canfd_ caps
    // it: a classic-only controller can never be asked to emit an FD frame.
    const bool send_fd = canfd_ && data.is_fdcan;
    frame.canfd_frame = send_fd;
    frame.bitrate_switch = send_fd;
    frame.rtr = data.is_remote_transmission;

    core::utility::assert_debug(data.can_data.size() <= 8);
    frame.dlc = data.can_data.size();
    if (!data.can_data.empty())
        std::memcpy(frame.data_8, data.can_data.data(), data.can_data.size());

    const hpm_stat_t status = mcan_transmit_via_txfifo_nonblocking(can_base_, &frame, nullptr);
    if (status != status_success)
        led::led->downlink_buffer_full();
}

ATTR_PLACE_AT(".fast")
void Can::handle_uplink(
    core::protocol::FieldId field_id, core::protocol::Serializer& serializer) {
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
    if (mcan_get_timestamp_from_received_message(can_base_, &rx, &ts_value) == status_success
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

ATTR_PLACE_AT(".fast")
void Can::irq_handler() {
    const uint32_t flags = mcan_get_interrupt_flags(can_base_);

    if (!flags) [[unlikely]]
        return;

    if (flags & MCAN_INT_RXFIFO0_NEW_MSG) [[likely]]
        handle_uplink(data_id_, usb::get_serializer());

    if (flags & (MCAN_INT_BUS_OFF_STATUS | MCAN_INT_WARNING_STATUS
                 | MCAN_INT_ERROR_PASSIVE
                 | MCAN_INT_PROTOCOL_ERR_IN_ARB_PHASE
                 | MCAN_INT_PROTOCOL_ERR_IN_DATA_PHASE)) {
        // Any error interrupt refreshes the indicator LED so it stays
        // visible while errors keep occurring.  Bus-off is the fatal state
        // and wins outright; otherwise the Last Error Code (LEC) names the
        // protocol error on the wire.  For CAN-FD the arbitration phase uses
        // LEC and the faster data phase uses DLEC -- prefer a concrete data-
        // phase code when the arbitration phase reports none.  A bus state
        // change (warning/passive) carrying no fresh LEC reports kNone, which
        // only refreshes the timeout (see Led::report_can_fault).
        led::CanFault fault = led::CanFault::kNone;
        if (mcan_is_in_busoff_state(can_base_)) {
            fault = led::CanFault::kBusOff;
            // Bus-off latches CCCR.INIT and halts the controller. This is a
            // forwarding bridge: a transient fault on the wire (downstream
            // node unplugged, no ACK) must not take the CAN port offline
            // until reboot. Clearing INIT starts the standard bus-off
            // recovery sequence -- the controller waits for the bus to go
            // idle (129 * 11 recessive bits), resets its error counters and
            // resumes automatically. While the bus stays faulty it simply
            // cycles back to bus-off and retries, keeping the port alive.
            mcan_enter_normal_mode(can_base_);
        } else {
            fault = classify_can_fault(mcan_get_last_error_code(can_base_));
            if (fault == led::CanFault::kNone)
                fault = classify_can_fault(mcan_get_data_phase_last_error_code(can_base_));
        }
        const uint8_t can_idx = (data_id_ == data::DataId::kCan0) ? 0U : 1U;
        led::led->report_can_fault(can_idx, fault);
    }

    mcan_clear_interrupt_flags(can_base_, flags);
}

} // namespace librmcs::firmware::can

namespace librmcs::firmware::board {

ATTR_PLACE_AT(".fast") void can_irq_handler(size_t board_can_index) {
    core::utility::assert_debug(board_can_index < can::kCanCount);

    can::can_array[board_can_index]->irq_handler();
}

} // namespace librmcs::firmware::board
