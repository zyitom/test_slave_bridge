#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <usart.h>

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/mc02/app/src/led/led.hpp"
#include "firmware/mc02/app/src/usb/helper.hpp"
#include "firmware/mc02/app/src/utility/lazy.hpp"

namespace librmcs::firmware::uart {

class Uart : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Uart, data::DataId, UART_HandleTypeDef*, uint16_t>;

    Uart(data::DataId data_id, UART_HandleTypeDef* hal_uart_handle, uint16_t max_receive_size)
        : data_id_(data_id)
        , hal_uart_handle_(hal_uart_handle)
        , max_receive_size_(max_receive_size) {
        core::utility::assert_always(max_receive_size_ <= 64);

        // CubeMX configures the RX DMA in circular mode, but we use the
        // idle-event + re-arm pattern, which needs one-shot (normal) mode. Force
        // it here so the receive path uses DMA without requiring a CubeMX change.
        // The DMA stream is already linked via __HAL_LINKDMA in MX_USARTx_Init.
        if (hal_uart_handle_->hdmarx != nullptr) {
            hal_uart_handle_->hdmarx->Init.Mode = DMA_NORMAL;
            core::utility::assert_always(HAL_DMA_Init(hal_uart_handle_->hdmarx) == HAL_OK);
        }

        core::utility::assert_always(trigger_hal_receive());
    }

    void handle_downlink(const data::UartDataView& data) {
        const auto size = data.uart_data.size();
        if (!size)
            return;

        auto writing = buffer_writing_.load(std::memory_order::relaxed);
        auto& buf = transmit_buffers_[writing];
        auto written = buf.written_size.load(std::memory_order::relaxed);

        const auto allowed = std::min(size, sizeof(buf.data) - written);
        if (allowed < size)
            led::led->downlink_buffer_full();

        if (allowed) {
            std::memcpy(&buf.data[written], data.uart_data.data(), allowed);
            buf.written_size.store(
                static_cast<uint8_t>(written + allowed), std::memory_order::relaxed);
        }
    }

    bool try_transmit() {
        // Poll-recover stuck reception
        if (hal_uart_handle_->RxState == HAL_UART_STATE_READY) [[unlikely]]
            trigger_hal_receive();

        const auto writing = buffer_writing_.load(std::memory_order::relaxed);
        if (transmit_buffers_[writing].written_size.load(std::memory_order::relaxed) == 0)
            return false;

        if (hal_uart_handle_->gState != HAL_UART_STATE_READY)
            return false;

        const auto next = static_cast<uint8_t>(!writing);
        transmit_buffers_[next].written_size.store(0, std::memory_order::relaxed);
        std::atomic_signal_fence(std::memory_order::release);
        buffer_writing_.store(next, std::memory_order::relaxed);
        std::atomic_signal_fence(std::memory_order::release);

        const auto tx_size =
            transmit_buffers_[writing].written_size.load(std::memory_order::relaxed);
        auto* tx_data = reinterpret_cast<uint8_t*>(transmit_buffers_[writing].data);

        // Prefer DMA when a TX DMA stream is linked (USART1/USART10/UART7); the
        // TX buffer lives in the MPU non-cacheable region, so no cache maintenance
        // is needed. UART5 (DBUS) is RX-only and has no TX DMA -> fall back to IT.
        const auto status = hal_uart_handle_->hdmatx != nullptr
            ? HAL_UART_Transmit_DMA(hal_uart_handle_, tx_data, tx_size)
            : HAL_UART_Transmit_IT(hal_uart_handle_, tx_data, tx_size);
        core::utility::assert_always(status == HAL_OK);

        return true;
    }

    void handle_uplink(uint16_t size, bool is_idle) {
        if (!size)
            return;

        auto& serializer = usb::get_serializer();
        core::utility::assert_always(
            serializer.write_uart(
                data_id_,
                {.uart_data = {receive_buffer_, size}, .idle_delimited = is_idle},
                {})
            != core::protocol::Serializer::SerializeResult::kInvalidArgument);

        trigger_hal_receive();
    }

    // HAL_UARTEx_RxEventCallback access
    friend void ::HAL_UARTEx_RxEventCallback(UART_HandleTypeDef*, uint16_t);

private:
    bool trigger_hal_receive() {
        // DMA receive (normal mode, see constructor): zero per-byte CPU; the idle
        // line still delimits frames at the same instant as before, so latency is
        // unchanged while the per-byte interrupt jitter is removed. The RX buffer
        // is in the MPU non-cacheable region, so no cache invalidation is needed.
        return HAL_UARTEx_ReceiveToIdle_DMA(
                   hal_uart_handle_,
                   reinterpret_cast<uint8_t*>(receive_buffer_),
                   max_receive_size_)
            == HAL_OK;
    }

    data::DataId data_id_;
    UART_HandleTypeDef* hal_uart_handle_;
    uint16_t max_receive_size_;

    std::byte receive_buffer_[64]{};

    struct {
        std::atomic<uint8_t> written_size = 0;
        std::byte data[128];
    } transmit_buffers_[2];
    std::atomic<uint8_t> buffer_writing_ = 0;
};

inline constinit Uart::Lazy uart1{data::DataId::kUart1, &huart1, 64};
inline constinit Uart::Lazy uart2{data::DataId::kUart2, &huart7, 64};
inline constinit Uart::Lazy uart3{data::DataId::kUart3, &huart10, 64};
inline constinit Uart::Lazy uart_dbus{data::DataId::kUartDbus, &huart5, 32};

} // namespace librmcs::firmware::uart
