#include "firmware/rmcs_board/app/src/can/can.hpp"

#include <algorithm>
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
bool Can::handle_uplink(
    core::protocol::FieldId field_id, core::protocol::Serializer& serializer) {
    mcan_rx_message_t rx;
    if (mcan_read_rxfifo(can_base_, 0, &rx) != status_success)
        return false;

    // rx.dlc is the raw 4-bit DLC from the wire, not a byte count, and every
    // value 0-15 can legally arrive from other bus nodes -- so it must be
    // normalized here instead of trusted downstream (the serializer rejects
    // out-of-contract views, and that rejection must not translate into an
    // assert on externally-controlled input):
    //   - Remote frames carry no data field; their DLC encodes the requested
    //     length, which the wire protocol cannot express. Forward them empty.
    //   - Classic frames may carry DLC 9-15, which the CAN spec says to treat
    //     as 8 data bytes.
    //   - FD frames with DLC > 8 hold 12-64 data bytes. The RX element data
    //     field is 8 bytes, so the hardware stored the payload truncated, and
    //     the wire protocol caps at 8 bytes anyway -- forwarding would deliver
    //     silently corrupted data. Drop the frame (but keep draining).
    if (rx.canfd_frame && rx.dlc > 8) [[unlikely]]
        return true;
    const size_t data_length = rx.rtr ? 0 : std::min<size_t>(rx.dlc, 8);

    data::CanDataView data;
    data.is_fdcan = rx.canfd_frame;
    data.is_extended_can_id = rx.use_ext_id;
    data.is_remote_transmission = rx.rtr;
    data.can_id = data.is_extended_can_id ? rx.ext_id : rx.std_id;
    data.can_data = {reinterpret_cast<const std::byte*>(rx.data_8), data_length};

    // 64-bit TSU timestamp captured at SOF from the shared PTPC0 timebase.
    // PTPC delivers an IEEE-1588 {seconds:nanoseconds} pair (high 32 bits =
    // seconds, low 32 bits = nanoseconds in [0, 1e9)); dividing reported
    // nanoseconds by kTsNsPerUs (= 960 at 160 MHz, not 1000) converts to
    // microseconds and undoes the PTPC digital-step error in one step.
    //
    // The conversion is deliberately 32-bit only: RV32 has no 64-bit divide
    // instruction, so a 64/32 division here would become a __udivdi3 library
    // loop (~hundreds of cycles) inside the highest-priority ISR. With the
    // seconds and nanoseconds words kept apart,
    //   us = sec * (1e9 / 960) + sec * (640 / 960 == 2/3) + ns / 960
    // needs only constant divisions, which GCC lowers to multiply-and-shift.
    // Versus the exact quotient this truncates at most 1 us per conversion
    // and the error does not accumulate. The result wraps every ~71.6 min;
    // the host only uses deltas, which are wrap-safe.
    // status_mcan_timestamp_not_exist (frame not matched by a sync filter)
    // leaves the field as std::nullopt.
    mcan_timestamp_value_t ts_value;
    if (mcan_get_timestamp_from_received_message(can_base_, &rx, &ts_value) == status_success
        && ts_value.is_64bit) {
        const auto sec = static_cast<uint32_t>(ts_value.ts_64bit >> 32);
        const auto ns = static_cast<uint32_t>(ts_value.ts_64bit);
        data.timestamp_us = sec * (1'000'000'000U / kTsNsPerUs) + (sec * 2U) / 3U
                          + ns / kTsNsPerUs;
    }

    const auto result = serializer.write_can(field_id, data);
    if (result == core::protocol::Serializer::SerializeResult::kBadAlloc) [[unlikely]]
        led::led->uplink_buffer_full();
    // Unreachable for wire input after the normalization above; guards only
    // against internal contract regressions.
    core::utility::assert_always(
        result != core::protocol::Serializer::SerializeResult::kInvalidArgument);

    return true;
}

ATTR_PLACE_AT(".fast")
void Can::irq_handler() {
    const uint32_t flags = mcan_get_interrupt_flags(can_base_);

    if (!flags) [[unlikely]]
        return;

    // Clear the flags before draining: IR is write-1-to-clear, and RF0N set by
    // a frame arriving mid-handler would be wiped by a clear at the end while
    // the frame stays in the FIFO -- stranded until the next frame happens to
    // arrive. Cleared up front, such a frame re-pends the interrupt and the
    // ISR simply runs again.
    mcan_clear_interrupt_flags(can_base_, flags);

    if (flags & MCAN_INT_RXFIFO0_NEW_MSG) [[likely]] {
        // Drain the FIFO completely: RF0N is a status bit, not a counter, so
        // one interrupt may stand for several buffered frames.
        auto& serializer = link::uplink_serializer();
        while (handle_uplink(data_id_, serializer)) {
        }
    }

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
}

} // namespace librmcs::firmware::can

namespace librmcs::firmware::board {

ATTR_PLACE_AT(".fast") void can_irq_handler(size_t board_can_index) {
    core::utility::assert_debug(board_can_index < can::kCanCount);

    can::can_array[board_can_index]->irq_handler();
}

} // namespace librmcs::firmware::board
