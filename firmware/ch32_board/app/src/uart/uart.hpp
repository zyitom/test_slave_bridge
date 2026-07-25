#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <array>

extern "C" {
#include "ch32h417.h"
}

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/ch32_board/app/src/board_app.hpp"
#include "firmware/ch32_board/app/src/led/led.hpp"
#include "firmware/ch32_board/app/src/link/uplink.hpp"
#include "firmware/ch32_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::uart {

// WCH USART forwarding driver, structured like upstream rmcs_board's Uart:
// board-agnostic (takes a board::UartPort, defers pins/AF/clock to
// board::init_uart), a double-buffered downlink, try_transmit() draining it, and
// a single irq_handler(). This board's first bring-up uses interrupt-mode RX
// (RXNE bytes + IDLE frame delimiter) and TX -- WCH ships no ReceiveToIdle-DMA
// helper; DMA + idle-line is a later optimization needing the DMAMUX mapping.
class Uart : private core::utility::Immovable {
public:
    using Lazy = utility::Lazy<Uart, board::UartPort>;

    explicit Uart(board::UartPort port)
        : data_id_(port.data_id)
        , uart_base_(reinterpret_cast<USART_TypeDef*>(port.base))
        , max_receive_size_(port.max_receive_size) {
        core::utility::assert_always(max_receive_size_ <= sizeof(receive_buffer_));
        board::init_uart(port);

        USART_InitTypeDef config = {};
        config.USART_BaudRate = port.baudrate;
        config.USART_WordLength = USART_WordLength_8b;
        config.USART_StopBits = USART_StopBits_1;
        config.USART_Parity = USART_Parity_No;
        config.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
        config.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
        USART_Init(uart_base_, &config);

        USART_ITConfig(uart_base_, USART_IT_RXNE, ENABLE);
        USART_ITConfig(uart_base_, USART_IT_IDLE, ENABLE);
        NVIC_EnableIRQ(port.irq_num);
        USART_Cmd(uart_base_, ENABLE);
    }

    [[nodiscard]] data::DataId data_id() const { return data_id_; }

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
        // An overrun leaves ORE latched and RXNE never asserts again, wedging RX
        // for good. The clear sequence is the classic read-STATR-then-read-DATAR;
        // the byte that caused it is already lost, so the partial frame is
        // flushed as non-idle-delimited and the host resynchronizes.
        if (USART_GetFlagStatus(uart_base_, USART_FLAG_ORE) != RESET) {
            (void)uart_base_->STATR;
            (void)USART_ReceiveData(uart_base_);
            handle_uplink(rx_count_, false);
            rx_count_ = 0;
        }

        if (USART_GetITStatus(uart_base_, USART_IT_RXNE) != RESET) {
            const auto byte = static_cast<std::byte>(USART_ReceiveData(uart_base_) & 0xFFu);
            receive_buffer_[rx_count_++] = byte;
            // Full buffer: flush now rather than dropping bytes on the floor.
            // Not idle-delimited -- the frame did not end, it just does not fit,
            // and the host must not treat the split as a frame boundary.
            if (rx_count_ >= max_receive_size_) {
                handle_uplink(rx_count_, false);
                rx_count_ = 0;
            }
        }

        if (USART_GetITStatus(uart_base_, USART_IT_IDLE) != RESET) {
            // Reading STATR then DATAR is the only way to clear IDLE. It also
            // clears RXNE, so any byte that landed since the branch above would
            // be consumed here -- harmless, because IDLE means the line has been
            // quiet for a full frame and no such byte exists.
            (void)uart_base_->STATR;
            (void)USART_ReceiveData(uart_base_);
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
        // Same gate as the CAN RX path: no telemetry before the session is up.
        if (!link::uplink_enabled())
            return;

        auto& serializer = link::uplink_serializer();
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

namespace internal {

template <size_t... I>
constexpr auto make_uart_array(std::index_sequence<I...> /*unused*/) {
    return std::array<Uart::Lazy, sizeof...(I)>{Uart::Lazy{board::kUartPorts[I]}...};
}

} // namespace internal

// Built straight from the board port table, as on rmcs_board: adding a port is a
// board_app.hpp table entry, nothing here changes.
inline constinit auto uart_array =
    internal::make_uart_array(std::make_index_sequence<board::kUartPortCount>{});
constexpr size_t kUartCount = board::kUartPortCount;

} // namespace librmcs::firmware::uart
