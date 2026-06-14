#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <fdcan.h>
#include <stm32h7xx_hal_fdcan.h>

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/mc02/app/src/led/led.hpp"
#include "firmware/mc02/app/src/utility/lazy.hpp"
#include "firmware/mc02/app/src/utility/ring_buffer.hpp"

namespace librmcs::firmware::can {

class Can : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Can, FDCAN_HandleTypeDef*, uint32_t>;

    Can(FDCAN_HandleTypeDef* hal_can_handle, uint32_t hal_filter_index)
        : hal_can_handle_(hal_can_handle) {
        config_can(hal_filter_index);
    }

    void handle_downlink(const data::CanDataView& data) {
        if (data.is_fdcan) [[unlikely]]
            return;

        auto construct = [&data](std::byte* storage) noexcept {
            auto& mailbox = *new (storage) TransmitMailboxData{};

            if (data.is_extended_can_id) {
                mailbox.identifier = (data.can_id << 0) | FDCAN_EXTENDED_ID;
            } else {
                mailbox.identifier = (data.can_id << 18) | FDCAN_STANDARD_ID;
            }
            mailbox.identifier |=
                data.is_remote_transmission ? FDCAN_REMOTE_FRAME : FDCAN_DATA_FRAME;

            core::utility::assert_debug(data.can_data.size() <= 8);
            mailbox.data_length = static_cast<uint32_t>(data.can_data.size());

            if (!data.can_data.empty())
                std::memcpy(mailbox.data, data.can_data.data(), data.can_data.size());
        };

        if (!transmit_buffer_.emplace_back_n(construct, 1))
            led::led->downlink_buffer_full();
    }

    void handle_uplink(data::DataId field_id, core::protocol::Serializer& serializer) {
        core::utility::assert_always(hal_can_handle_->State == HAL_FDCAN_STATE_BUSY);
        auto* hal_can_instance = hal_can_handle_->Instance;

        struct RxMailbox {
            uint32_t RIR;
            uint32_t RDTR;
            uint32_t RDLR;
            uint32_t RDHR;
        };

        // Drain the entire Rx FIFO0 in this single interrupt: process every queued
        // message now instead of taking a fresh interrupt per message. This does
        // NOT add latency -- the interrupt still fires on the first new message, so
        // the first message is handled just as fast; the loop only mops up messages
        // that piled up during processing, which would otherwise each cost another
        // ISR entry/exit. Net effect is lower burst latency, never higher.
        while ((hal_can_instance->RXF0S & FDCAN_RXF0S_F0FL) != 0U) {
            const auto get_index =
                (hal_can_instance->RXF0S & FDCAN_RXF0S_F0GI) >> FDCAN_RXF0S_F0GI_Pos;

            auto* rx_mailbox = reinterpret_cast<RxMailbox*>(
                hal_can_handle_->msgRam.RxFIFO0SA
                + (get_index * hal_can_handle_->Init.RxFifo0ElmtSize * 4U));

            data::CanDataView can_data{};
            can_data.is_fdcan = false;
            can_data.is_extended_can_id = static_cast<bool>(rx_mailbox->RIR & 0x40000000U);
            can_data.is_remote_transmission = static_cast<bool>(rx_mailbox->RIR & 0x20000000U);

            if (can_data.is_extended_can_id) {
                can_data.can_id = rx_mailbox->RIR & 0x1FFFFFFFU;
            } else {
                can_data.can_id = (rx_mailbox->RIR & 0x1FFC0000U) >> 18;
            }

            size_t can_data_length = (rx_mailbox->RDTR & 0x000F0000U) >> 16;
            if (can_data.is_remote_transmission)
                can_data_length = 0;

            alignas(uint32_t) std::array<std::byte, 8> payload{};
            const uint32_t rdlr = rx_mailbox->RDLR;
            const uint32_t rdhr = rx_mailbox->RDHR;
            std::memcpy(payload.data(), &rdlr, sizeof(uint32_t));
            std::memcpy(payload.data() + 4, &rdhr, sizeof(uint32_t));
            can_data.can_data = {payload.data(), can_data_length};

            core::utility::assert_always(
                serializer.write_can(field_id, can_data)
                != core::protocol::Serializer::SerializeResult::kInvalidArgument);

            hal_can_instance->RXF0A = get_index;
        }
    }

    bool try_transmit() {
        auto* hcan = hal_can_handle_;

        core::utility::assert_always(hcan->State == HAL_FDCAN_STATE_BUSY);

        const uint32_t txfqs = hcan->Instance->TXFQS;
        const auto free_mailbox_count = txfqs & FDCAN_TXFQS_TFFL;

        return transmit_buffer_.pop_front_n(
            [this, hcan](const TransmitMailboxData& mailbox_data) noexcept {
                const auto put_index =
                    (hcan->Instance->TXFQS & FDCAN_TXFQS_TFQPI) >> FDCAN_TXFQS_TFQPI_Pos;

                struct TxMailbox {
                    uint32_t TIR;
                    uint32_t TDTR;
                    uint32_t TDLR;
                    uint32_t TDHR;
                };
                auto* target_mailbox = reinterpret_cast<TxMailbox*>(
                    hcan->msgRam.TxBufferSA
                    + (put_index * hcan->Init.TxElmtSize * 4U));

                target_mailbox->TIR = mailbox_data.identifier;
                target_mailbox->TDTR = mailbox_data.data_length << 16;
                target_mailbox->TDLR = mailbox_data.data[0];
                target_mailbox->TDHR = mailbox_data.data[1];

                hcan->Instance->TXBAR = (1UL << put_index);
                hcan->LatestTxFifoQRequest = (1UL << put_index);
            },
            free_mailbox_count);
    }

private:
    void config_can(uint32_t hal_filter_index) {
        FDCAN_FilterTypeDef filter_config;

        filter_config.IdType = FDCAN_STANDARD_ID;
        filter_config.FilterIndex = hal_filter_index;
        filter_config.FilterType = FDCAN_FILTER_MASK;
        filter_config.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
        filter_config.FilterID1 = 0x0000;
        filter_config.FilterID2 = 0x0000;

        constexpr auto ok = HAL_OK;
        core::utility::assert_always(HAL_FDCAN_ConfigFilter(hal_can_handle_, &filter_config) == ok);

        filter_config.IdType = FDCAN_EXTENDED_ID;
        filter_config.FilterIndex = hal_filter_index + 1;
        filter_config.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;

        core::utility::assert_always(HAL_FDCAN_ConfigFilter(hal_can_handle_, &filter_config) == ok);

        core::utility::assert_always(HAL_FDCAN_Start(hal_can_handle_) == ok);
        core::utility::assert_always(
            HAL_FDCAN_ActivateNotification(
                hal_can_handle_, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0)
            == ok);
    }

    FDCAN_HandleTypeDef* hal_can_handle_;

    struct TransmitMailboxData {
        uint32_t identifier;
        uint32_t data_length;
        uint32_t data[2];
    };
    utility::RingBuffer<TransmitMailboxData, 16> transmit_buffer_;
};

inline constinit Can::Lazy can1{&hfdcan1, 0};
inline constinit Can::Lazy can2{&hfdcan2, 0};
inline constinit Can::Lazy can3{&hfdcan3, 0};

} // namespace librmcs::firmware::can
