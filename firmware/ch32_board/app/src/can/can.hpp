#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

extern "C" {
#include "ch32h417.h"
}

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/protocol.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/ch32_board/app/src/board_app.hpp"
#include "firmware/ch32_board/app/src/led/led.hpp"
#include "firmware/ch32_board/app/src/usb/helper.hpp"
#include "firmware/ch32_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::can {

struct HardwareConfig {
    uint32_t base;
    IRQn_Type irq_num;
};

// Classic bxCAN forwarding driver, structured like upstream rmcs_board's Can:
// board-agnostic (takes a HardwareConfig, defers pins/clock to board::init_can),
// downlink transmits DIRECTLY to the controller's hardware TX mailboxes (no
// software TX ring buffer, no try_transmit -- bxCAN has three TX mailboxes that
// serve that role), and a single irq_handler() drains RX FIFO0. CH32H417 CAN is
// CAN 2.0B only, so is_fdcan is always false.
class Can : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Can, data::DataId, HardwareConfig>;

    explicit Can(data::DataId data_id, HardwareConfig board_config)
        : data_id_(data_id)
        , can_base_(reinterpret_cast<CAN_TypeDef*>(board_config.base)) {
        const uint32_t can_clock = board::init_can(can_base_);

        CAN_InitTypeDef config = {};
        config.CAN_TTCM = DISABLE;
        config.CAN_ABOM = ENABLE; // auto bus-off recovery
        config.CAN_AWUM = DISABLE;
        config.CAN_NART = DISABLE;
        config.CAN_RFLM = DISABLE;
        config.CAN_TXFP = DISABLE;
        config.CAN_Mode = CAN_Mode_Normal;
        config.CAN_SJW = CAN_SJW_1tq;
        config.CAN_BS1 = CAN_BS1_6tq;
        config.CAN_BS2 = CAN_BS2_1tq;
        // 8 tq/bit -> 1 Mbit/s. TODO(bring-up): confirm the CAN kernel-clock
        // divider (board::init_can currently returns SystemCoreClock).
        config.CAN_Prescaler = static_cast<uint16_t>(can_clock / (1'000'000u * 8u));
        CAN_Init(can_base_, &config);

        CAN_FilterInitTypeDef filter = {};
        filter.CAN_FilterNumber = (can_base_ == CAN1) ? 0 : 14;
        filter.CAN_FilterMode = CAN_FilterMode_IdMask;
        filter.CAN_FilterScale = CAN_FilterScale_32bit;
        filter.CAN_FilterIdHigh = 0x0000;
        filter.CAN_FilterIdLow = 0x0000;
        filter.CAN_FilterMaskIdHigh = 0x0000;
        filter.CAN_FilterMaskIdLow = 0x0000;
        filter.CAN_FilterFIFOAssignment = CAN_FIFO0;
        filter.CAN_FilterActivation = ENABLE;
        CAN_FilterInit(&filter);

        CAN_ITConfig(can_base_, CAN_IT_FMP0, ENABLE);
        NVIC_EnableIRQ(board_config.irq_num);
    }

    void handle_downlink(const data::CanDataView& data) {
        CanTxMsg msg = {};
        if (data.is_extended_can_id) {
            msg.IDE = CAN_Id_Extended;
            msg.ExtId = data.can_id & 0x1FFFFFFFu;
        } else {
            msg.IDE = CAN_Id_Standard;
            msg.StdId = data.can_id & 0x7FFu;
        }
        msg.RTR = data.is_remote_transmission ? CAN_RTR_Remote : CAN_RTR_Data;

        core::utility::assert_debug(data.can_data.size() <= 8);
        msg.DLC = static_cast<uint8_t>(data.can_data.size());
        if (!data.can_data.empty())
            std::memcpy(msg.Data, data.can_data.data(), data.can_data.size());

        // Direct to a hardware TX mailbox; NO_MB means all three are busy -> drop.
        if (CAN_Transmit(can_base_, &msg) == CAN_TxStatus_NoMailBox)
            led::led->downlink_buffer_full();
    }

    void handle_uplink(core::protocol::Serializer& serializer) {
        while (CAN_MessagePending(can_base_, CAN_FIFO0) > 0) {
            CanRxMsg rx = {};
            CAN_Receive(can_base_, CAN_FIFO0, &rx);

            data::CanDataView data;
            data.is_fdcan = false;
            data.is_extended_can_id = (rx.IDE == CAN_Id_Extended);
            data.is_remote_transmission = (rx.RTR == CAN_RTR_Remote);
            data.can_id = data.is_extended_can_id ? rx.ExtId : rx.StdId;

            size_t length = data.is_remote_transmission ? 0 : rx.DLC;
            if (length > 8)
                length = 8;
            data.can_data = {reinterpret_cast<const std::byte*>(rx.Data), length};

            core::utility::assert_always(
                serializer.write_can(data_id_, data)
                != core::protocol::Serializer::SerializeResult::kInvalidArgument);
        }
    }

    void irq_handler() { handle_uplink(usb::get_serializer()); }

private:
    const data::DataId data_id_;
    CAN_TypeDef* can_base_;
};

constexpr HardwareConfig kBoardConfigs[] = {
    {.base = CAN1_BASE, .irq_num = CAN1_RX0_IRQn},
    {.base = CAN2_BASE, .irq_num = CAN2_RX0_IRQn},
};

inline constinit Can::Lazy can_array[]{
    Can::Lazy{data::DataId::kCan1, kBoardConfigs[0]},
    Can::Lazy{data::DataId::kCan2, kBoardConfigs[1]},
};
constexpr size_t kCanCount = std::size(can_array);

} // namespace librmcs::firmware::can
