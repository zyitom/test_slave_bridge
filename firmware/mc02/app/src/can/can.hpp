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

        // Extended-ID frames are accepted through the global filter below, NOT a
        // dedicated filter element: CubeMX configures ExtFiltersNbr = 0, so no
        // extended filter list exists in the message RAM -- the HAL validates the
        // index only via assert_param (compiled out), so a ConfigFilter call here
        // would silently scribble the element into RX FIFO0's RAM area and never
        // take effect. Making the accept-all policy explicit in GFC also stops
        // relying on the register's reset value. Remote frames keep passing
        // through normal filtering; handle_uplink() normalizes them to empty.
        core::utility::assert_always(
            HAL_FDCAN_ConfigGlobalFilter(
                hal_can_handle_, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE)
            == ok);

        // Hardware RX timestamping is disabled: the internal counter is 16 bits
        // wide, which wraps every ~65.5 ms and cannot carry the 32-bit microsecond
        // timestamp the protocol specifies. handle_uplink() leaves
        // CanDataView::timestamp_us unset, so the counter would only burn bus
        // cycles. Re-enable both calls here if the uplink ever reports timestamps
        // again; they must run while the controller is still in READY state
        // (before HAL_FDCAN_Start).
        //
        // Internal 16-bit counter, one tick = one nominal CAN bit time = 1 us at
        // the 1 Mbit/s arbitration rate.
        // core::utility::assert_always(
        //     HAL_FDCAN_ConfigTimestampCounter(hal_can_handle_, FDCAN_TIMESTAMP_PRESC_1) == ok);
        // core::utility::assert_always(
        //     HAL_FDCAN_EnableTimestampCounter(hal_can_handle_, FDCAN_TIMESTAMP_INTERNAL) == ok);

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

    // Logical index for telemetry, derived from the peripheral instance rather
    // than stored: the constructor takes no index, and adding one would touch
    // every call site for a diagnostics-only value.
    [[nodiscard]] std::size_t diag_index() const {
        if (hal_can_handle_->Instance == FDCAN1)
            return 0;
        if (hal_can_handle_->Instance == FDCAN2)
            return 1;
        return 2;
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
