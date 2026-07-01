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

    // CAN forwarding hot path. Defined out-of-line in can.cpp and placed in the
    // zero-wait ITCM (.itcm section) to keep worst-case forwarding latency free of
    // I-cache misses and FLASH-XIP fetch jitter. Out-of-line (not inline-in-class)
    // is deliberate: inline/COMDAT bodies in a custom section trip a GCC section
    // type conflict, so the bodies live in the .cpp like the ISR callbacks.
    void handle_downlink(const data::CanDataView& data);
    void handle_uplink(data::DataId field_id, core::protocol::Serializer& serializer);
    bool try_transmit();

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

        // Hardware RX timestamping: internal 16-bit counter, one tick = one nominal
        // CAN bit time = 1 us at the 1 Mbit/s arbitration rate. Must be configured
        // while the controller is still in READY state (before HAL_FDCAN_Start).
        core::utility::assert_always(
            HAL_FDCAN_ConfigTimestampCounter(hal_can_handle_, FDCAN_TIMESTAMP_PRESC_1) == ok);
        core::utility::assert_always(
            HAL_FDCAN_EnableTimestampCounter(hal_can_handle_, FDCAN_TIMESTAMP_INTERNAL) == ok);

        core::utility::assert_always(HAL_FDCAN_Start(hal_can_handle_) == ok);
        core::utility::assert_always(
            HAL_FDCAN_ActivateNotification(
                hal_can_handle_, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0)
            == ok);
        // Bus-off recovery: this is a forwarding bridge, so a transient wire fault
        // (downstream node unplugged, no ACK) must not park the port until reboot.
        // The notification drives HAL_FDCAN_ErrorStatusCallback (see can.cpp), which
        // restarts bus-off recovery so the port comes back on its own.
        core::utility::assert_always(
            HAL_FDCAN_ActivateNotification(hal_can_handle_, FDCAN_IT_BUS_OFF, 0) == ok);
    }

    FDCAN_HandleTypeDef* hal_can_handle_;

    struct TransmitMailboxData {
        uint32_t identifier; // Tx element T0: ID + XTD/RTR flags
        uint32_t control;    // Tx element T1: DLC + FDF/BRS flags
        uint32_t data[2];
    };
    utility::RingBuffer<TransmitMailboxData, 16> transmit_buffer_;
};

// In zero-wait DTCM (.dtcm): the RX ISR reads hal_can_handle_/data_id_ from here,
// and the TX ring lives here -- keeps the forwarding hot path off the AXI bus.
[[gnu::section(".dtcm")]] inline constinit Can::Lazy can1{&hfdcan1, 0};
[[gnu::section(".dtcm")]] inline constinit Can::Lazy can2{&hfdcan2, 0};
[[gnu::section(".dtcm")]] inline constinit Can::Lazy can3{&hfdcan3, 0};

} // namespace librmcs::firmware::can
