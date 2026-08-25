#include "firmware/mc02/app/src/can/can.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <fdcan.h>

#include "core/include/librmcs/data/datas.hpp"
#include "firmware/mc02/app/src/diag/can_diag.hpp"
#include "firmware/mc02/app/src/led/led.hpp"
#include "firmware/mc02/app/src/usb/helper.hpp"

// Place the CAN forwarding hot path in zero-wait ITCM (copied from FLASH at boot
// by app.cpp; see the .itcm linker section). This removes I-cache misses and
// FLASH-XIP fetch jitter from the worst-case forwarding latency. Leaf calls into
// the HAL and the shared serializer stay in FLASH -- only this glue runs from
// ITCM. Bodies are kept out-of-line here (not inline-in-class) because an
// inline/COMDAT function in a custom section trips a GCC section type conflict.
#define LIBRMCS_ITCM __attribute__((section(".itcm")))

namespace librmcs::firmware::can {

LIBRMCS_ITCM
uint32_t Can::hardware_free_slots() const noexcept {
    return hal_can_handle_->Instance->TXFQS & FDCAN_TXFQS_TFFL;
}

// Write one element into the Tx FIFO/queue at the controller's current put index
// and request its transmission. The caller must have checked hardware_free_slots()
// first: with the FIFO full, TFQPI still reads back a slot that is waiting to go
// out, so writing here would overwrite an unsent frame.
LIBRMCS_ITCM
void Can::push_to_hardware(const TransmitMailboxData& mailbox_data) noexcept {
    auto* hcan = hal_can_handle_;
    const auto put_index =
        (hcan->Instance->TXFQS & FDCAN_TXFQS_TFQPI) >> FDCAN_TXFQS_TFQPI_Pos;

    struct TxMailbox {
        uint32_t TIR;
        uint32_t TDTR;
        uint32_t TDLR;
        uint32_t TDHR;
    };
    auto* target_mailbox = reinterpret_cast<TxMailbox*>(
        hcan->msgRam.TxBufferSA + (put_index * hcan->Init.TxElmtSize * 4U));

    target_mailbox->TIR = mailbox_data.identifier;
    target_mailbox->TDTR = mailbox_data.control;
    target_mailbox->TDLR = mailbox_data.data[0];
    target_mailbox->TDHR = mailbox_data.data[1];

    hcan->Instance->TXBAR = (1UL << put_index);
    hcan->LatestTxFifoQRequest = (1UL << put_index);
}

LIBRMCS_ITCM
void Can::handle_downlink(const data::CanDataView& data) {
    TransmitMailboxData mailbox{};

    if (data.is_extended_can_id) {
        mailbox.identifier = (data.can_id << 0) | FDCAN_EXTENDED_ID;
    } else {
        mailbox.identifier = (data.can_id << 18) | FDCAN_STANDARD_ID;
    }
    mailbox.identifier |= data.is_remote_transmission ? FDCAN_REMOTE_FRAME : FDCAN_DATA_FRAME;

    core::utility::assert_debug(data.can_data.size() <= 8);
    const auto dlc = static_cast<uint32_t>(data.can_data.size());

    // The controller stays permanently in FD+BRS mode, a strict superset of
    // classic CAN: the per-frame FDF/BRS bits in the Tx element (T1) select the
    // format, so a classic frame (is_fdcan == false) goes out with FDF/BRS clear
    // and an FD frame switches to the 5 Mbit/s data phase -- no INIT-mode
    // reconfiguration, just two extra bits on the hot path.
    mailbox.control = dlc << 16;
    if (data.is_fdcan)
        mailbox.control |= FDCAN_FD_CAN | FDCAN_BRS_ON;

    if (!data.can_data.empty())
        std::memcpy(mailbox.data, data.can_data.data(), data.can_data.size());

    // Straight to the controller while it has room, and queue only behind a full
    // Tx FIFO. This is reached from tud_vendor_rx_cb inside tud_task(), so going
    // through the queue unconditionally -- what this did before -- made every
    // frame wait for the try_transmit() call at the far end of the main loop,
    // behind the DFU poll, the GPIO sampling, one BMI088 SPI read and the LED
    // poll (a WS2812 refresh is ~330 us when the colour changes). That whole
    // stretch was added to one-way latency and, worse, its length varies from
    // pass to pass, which is where the long tail came from.
    //
    // It also cost burst capacity: the queue is drained into the FIFO only by
    // try_transmit(), so every frame of one USB packet landed in the queue and
    // nothing reached the 32-element FIFO until the packet was fully parsed.
    // The effective limit was the queue depth alone -- measured at exactly
    // min(N, 16) frames delivered per downlink packet.
    //
    // The queue must not be bypassed once it is non-empty, or a frame written
    // here would overtake one already waiting. Checking peek_front() from the
    // producer is safe despite RingBuffer's consumer-only note: producer
    // (handle_downlink, reached from tud_task) and consumer (try_transmit) both
    // run on the main loop, on the same thread.
    if (transmit_buffer_.peek_front() == nullptr && hardware_free_slots() != 0) {
        push_to_hardware(mailbox);
        return;
    }

    const auto copy = [&mailbox](std::byte* storage) noexcept {
        *new (storage) TransmitMailboxData{mailbox};
    };
    if (!transmit_buffer_.emplace_back_n(copy, 1)) {
        led::led->downlink_buffer_full();
        diag::note_tx_fail(diag_index());
    }
}

LIBRMCS_ITCM
void Can::handle_uplink(data::DataId field_id, core::protocol::Serializer& serializer) {
    core::utility::assert_always(hal_can_handle_->State == HAL_FDCAN_STATE_BUSY);
    auto* hal_can_instance = hal_can_handle_->Instance;

    struct RxMailbox {
        uint32_t RIR;
        uint32_t RDTR;
        uint32_t RDLR;
        uint32_t RDHR;
    };

    // Drain the entire Rx FIFO0 in this single interrupt: process every queued
    // message now instead of taking a fresh interrupt per message. This does NOT
    // add latency -- the interrupt still fires on the first new message, so the
    // first message is handled just as fast; the loop only mops up messages that
    // piled up during processing, which would otherwise each cost another ISR
    // entry/exit. Net effect is lower burst latency, never higher.
    while ((hal_can_instance->RXF0S & FDCAN_RXF0S_F0FL) != 0U) {
        const auto get_index = (hal_can_instance->RXF0S & FDCAN_RXF0S_F0GI) >> FDCAN_RXF0S_F0GI_Pos;

        auto* rx_mailbox = reinterpret_cast<RxMailbox*>(
            hal_can_handle_->msgRam.RxFIFO0SA
            + (get_index * hal_can_handle_->Init.RxFifo0ElmtSize * 4U));

        const uint32_t rdtr = rx_mailbox->RDTR;
        data::CanDataView can_data{};
        // Rx element R1 bit 21 (FDF) marks an FD frame; the forwarder mirrors it so
        // the host learns each frame's true format.
        can_data.is_fdcan = static_cast<bool>(rdtr & FDCAN_FD_CAN);
        can_data.is_extended_can_id = static_cast<bool>(rx_mailbox->RIR & 0x40000000U);
        can_data.is_remote_transmission = static_cast<bool>(rx_mailbox->RIR & 0x20000000U);

        if (can_data.is_extended_can_id) {
            can_data.can_id = rx_mailbox->RIR & 0x1FFFFFFFU;
        } else {
            can_data.can_id = (rx_mailbox->RIR & 0x1FFC0000U) >> 18;
        }

        // Hardware RX timestamping is disabled on this board: the FDCAN internal
        // counter is only 16 bits wide, so the reported value wraps every ~65.5 ms
        // and does not satisfy the 32-bit microsecond contract of
        // CanDataView::timestamp_us (see core/include/librmcs/data/datas.hpp). Left
        // unset, the serializer omits the field entirely and saves 4 bytes per
        // uplink frame. To re-enable, restore the line below together with the
        // FDCAN_TIMESTAMP_* configuration in config_can() -- and widen the value
        // first (e.g. fold in the free-running TIM5 microsecond counter) so hosts
        // can keep using plain 32-bit wrapping deltas.
        //
        // 16-bit hardware timestamp captured at start-of-frame (R1 bits[15:0]). The
        // internal counter ticks once per nominal CAN bit time, which is 1 us at the
        // 1 Mbit/s arbitration rate (prescaler 1), so the value is already in
        // microseconds.
        // can_data.timestamp_us = static_cast<uint32_t>(rdtr & 0x0000FFFFU);

        size_t can_data_length = (rdtr & 0x000F0000U) >> 16;
        if (can_data.is_remote_transmission)
            can_data_length = 0;
        // The DLC arrives raw from the wire. An FD frame with DLC 9..15 holds
        // 12..64 data bytes, but the RX element stores only 8 of them
        // (FDCAN_DATA_BYTES_8) and the wire protocol caps at 8 anyway, so
        // forwarding would deliver silently truncated data -- drop the frame
        // instead, but keep draining the FIFO. A classic frame with DLC 9..15
        // is legal and means 8 data bytes per the CAN spec.
        if (can_data.is_fdcan && can_data_length > 8) [[unlikely]] {
            hal_can_instance->RXF0A = get_index;
            continue;
        }
        can_data_length = std::min<size_t>(can_data_length, 8);

        alignas(uint32_t) std::array<std::byte, 8> payload{};
        const uint32_t rdlr = rx_mailbox->RDLR;
        const uint32_t rdhr = rx_mailbox->RDHR;
        std::memcpy(payload.data(), &rdlr, sizeof(uint32_t));
        std::memcpy(payload.data() + 4, &rdhr, sizeof(uint32_t));
        can_data.can_data = {payload.data(), can_data_length};

        // kBadAlloc means the uplink batch pool was full and this frame did NOT
        // get serialized. It used to go unchecked while RXF0A below acknowledged
        // the message anyway, so the frame was dropped with nothing recorded
        // anywhere -- no counter, no LED, no way for the host to tell a dropped
        // frame from one that never arrived. On this Full-Speed board (1 ms
        // frames) that is reachable under ordinary load: with mc02's CAN2<->CAN3
        // strap, one downlink packet carrying two CAN fields makes each
        // controller receive both frames, so 2 frames sent become 4 to ship
        // upstream per round and the pool runs dry. Measured as ~30% of the
        // frames from whichever CAN field came *second* in the packet silently
        // vanishing, which looked like a transmit or arbitration fault and is
        // neither.
        //
        // Matches rmcs_board's Can::serialize_uplink, which has always flagged
        // this. Still acknowledged below either way: retrying from the RX FIFO
        // would stall the drain loop and cost newer frames too, so a full pool
        // drops the frame -- the point here is that it stops being silent.
        const auto uplink_result = serializer.write_can(field_id, can_data);
        if (uplink_result == core::protocol::Serializer::SerializeResult::kBadAlloc) [[unlikely]] {
            led::led->uplink_buffer_full();
            diag::note_uplink_drop(diag_index());
        } else {
            diag::note_frame(diag_index());
        }
        core::utility::assert_always(
            uplink_result != core::protocol::Serializer::SerializeResult::kInvalidArgument);

        hal_can_instance->RXF0A = get_index;
    }
}

LIBRMCS_ITCM
bool Can::try_transmit() {
    core::utility::assert_always(hal_can_handle_->State == HAL_FDCAN_STATE_BUSY);

    // Only frames that found the FIFO full are here now; the common case is an
    // empty queue and an early return of false after two loads.
    return transmit_buffer_.pop_front_n(
        [this](const TransmitMailboxData& mailbox_data) noexcept {
            push_to_hardware(mailbox_data);
        },
        hardware_free_slots());
}

extern "C" LIBRMCS_ITCM void HAL_FDCAN_RxFifo0Callback(
    FDCAN_HandleTypeDef* hfdcan, uint32_t rx_fifo0_its) {
    (void)rx_fifo0_its;

    Can* can;
    data::DataId field_id;
    std::size_t diag_index;

    if (hfdcan == &hfdcan1) {
        can = can1.get();
        field_id = data::DataId::kCan1;
        diag_index = 0;
    } else if (hfdcan == &hfdcan2) {
        can = can2.get();
        field_id = data::DataId::kCan2;
        diag_index = 1;
    } else if (hfdcan == &hfdcan3) {
        can = can3.get();
        field_id = data::DataId::kCan3;
        diag_index = 2;
    } else {
        return;
    }

    // Counted per interrupt, not per frame: a frozen entry count next to a
    // non-empty RX FIFO is what distinguishes "the interrupt stopped being
    // delivered" from "the controller stopped seeing traffic".
    diag::note_isr_entry(diag_index);

    can->handle_uplink(field_id, usb::get_serializer());
}

extern "C" void HAL_FDCAN_ErrorStatusCallback(
    FDCAN_HandleTypeDef* hfdcan, uint32_t error_status_its) {
    if (!(error_status_its & FDCAN_IT_BUS_OFF))
        return;

    FDCAN_ProtocolStatusTypeDef status;
    HAL_FDCAN_GetProtocolStatus(hfdcan, &status);
    if (status.BusOff == 0U)
        return;

    // On bus-off the M_CAN core sets CCCR.INIT of its own accord and halts. The HAL
    // state stays BUSY, so HAL_FDCAN_Start() would refuse -- clear INIT directly to
    // launch the standard recovery sequence: the controller waits for 129 * 11
    // consecutive recessive bits, resets its error counters and resumes. While the
    // bus stays faulty it just cycles back to bus-off and retries, keeping the port
    // alive without a reboot.
    CLEAR_BIT(hfdcan->Instance->CCCR, FDCAN_CCCR_INIT);
}

} // namespace librmcs::firmware::can
