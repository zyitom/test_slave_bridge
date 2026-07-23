#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

extern "C" {
#include "ch32h417.h"
}

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/ch32_board/app/src/board_app.hpp"
#include "firmware/ch32_board/app/src/led/led.hpp"
#include "firmware/ch32_board/app/src/usb/helper.hpp"
#include "firmware/ch32_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::uart {

struct HardwareConfig {
    uint32_t base;
    IRQn_Type irq_num;
};

// WCH USART forwarding driver, structured like upstream rmcs_board's Uart:
// board-agnostic (HardwareConfig + board::init_uart), a double-buffered downlink,
// try_transmit() draining it, and a single irq_handler(). This board's first
// bring-up uses interrupt-mode RX (RXNE bytes + IDLE frame delimiter) and TX
// (WCH has no ReceiveToIdle-DMA helper); DMA is a later optimization.
class Uart : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Uart, data::DataId, HardwareConfig, uint32_t>;

    explicit Uart(data::DataId data_id, HardwareConfig board_config, uint16_t max_receive_size)
        : data_id_(data_id)
        , uart_base_(reinterpret_cast<USART_TypeDef*>(board_config.base))
        , max_receive_size_(max_receive_size) {
        core::utility::assert_always(max_receive_size_ <= sizeof(receive_buffer_));
        board::init_uart(uart_base_);

        USART_InitTypeDef config = {};
        config.USART_BaudRate = 115200;
        config.USART_WordLength = USART_WordLength_8b;
        config.USART_StopBits = USART_StopBits_1;
        config.USART_Parity = USART_Parity_No;
        config.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
        config.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
        USART_Init(uart_base_, &config);

        USART_ITConfig(uart_base_, USART_IT_RXNE, ENABLE);
        USART_ITConfig(uart_base_, USART_IT_IDLE, ENABLE);
        NVIC_EnableIRQ(board_config.irq_num);
        USART_Cmd(uart_base_, ENABLE);
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

    void try_transmit() {
        if (tx_busy_.load(std::memory_order::relaxed))
            return;

        const auto writing = buffer_writing_.load(std::memory_order::relaxed);
        if (transmit_buffers_[writing].written_size.load(std::memory_order::relaxed) == 0)
            return;

        const auto next = static_cast<uint8_t>(!writing);
        transmit_buffers_[next].written_size.store(0, std::memory_order::relaxed);
        std::atomic_signal_fence(std::memory_order::release);
        buffer_writing_.store(next, std::memory_order::relaxed);
        std::atomic_signal_fence(std::memory_order::release);

        tx_size_ = transmit_buffers_[writing].written_size.load(std::memory_order::relaxed);
        tx_index_ = 0;
        tx_source_ = writing;
        tx_busy_.store(true, std::memory_order::relaxed);

        USART_ITConfig(uart_base_, USART_IT_TXE, ENABLE);
    }

    void irq_handler() {
        if (USART_GetITStatus(uart_base_, USART_IT_RXNE) != RESET) {
            const auto byte = static_cast<std::byte>(USART_ReceiveData(uart_base_) & 0xFFu);
            if (rx_count_ < max_receive_size_)
                receive_buffer_[rx_count_++] = byte;
        }
        if (USART_GetITStatus(uart_base_, USART_IT_IDLE) != RESET) {
            (void)uart_base_->STATR;
            (void)USART_ReceiveData(uart_base_); // clear IDLE
            handle_uplink(rx_count_, true);
            rx_count_ = 0;
        }
        if (USART_GetITStatus(uart_base_, USART_IT_TXE) != RESET) {
            if (tx_index_ < tx_size_) {
                USART_SendData(
                    uart_base_,
                    static_cast<uint16_t>(transmit_buffers_[tx_source_].data[tx_index_++]));
            } else {
                USART_ITConfig(uart_base_, USART_IT_TXE, DISABLE);
                tx_busy_.store(false, std::memory_order::relaxed);
            }
        }
    }

private:
    void handle_uplink(uint16_t size, bool is_idle) {
        if (!size)
            return;

        auto& serializer = usb::get_serializer();
        core::utility::assert_always(
            serializer.write_uart(
                data_id_,
                {.uart_data = {receive_buffer_, size}, .idle_delimited = is_idle}, {})
            != core::protocol::Serializer::SerializeResult::kInvalidArgument);
    }

    const data::DataId data_id_;
    USART_TypeDef* uart_base_;
    uint16_t max_receive_size_;

    std::byte receive_buffer_[64]{};
    uint16_t rx_count_ = 0;

    struct {
        std::atomic<uint8_t> written_size = 0;
        std::byte data[128];
    } transmit_buffers_[2];
    std::atomic<uint8_t> buffer_writing_ = 0;

    std::atomic<bool> tx_busy_ = false;
    uint16_t tx_index_ = 0;
    uint16_t tx_size_ = 0;
    uint8_t tx_source_ = 0;
};

constexpr HardwareConfig kBoardConfigs[] = {
    {.base = USART1_BASE, .irq_num = USART1_IRQn},
    {.base = USART2_BASE, .irq_num = USART2_IRQn},
};

inline constinit Uart::Lazy uart_array[]{
    Uart::Lazy{data::DataId::kUart1, kBoardConfigs[0], 64},
    Uart::Lazy{data::DataId::kUart2, kBoardConfigs[1], 64},
};
constexpr size_t kUartCount = std::size(uart_array);

} // namespace librmcs::firmware::uart
