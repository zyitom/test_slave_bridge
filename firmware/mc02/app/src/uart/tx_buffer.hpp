#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <main.h>
#include <usart.h>

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/utility/assert.hpp"
#include "firmware/mc02/app/src/timer/timer.hpp"
#include "firmware/mc02/app/src/utility/ring_buffer.hpp"

namespace librmcs::firmware::uart {

// Ring-buffered DMA transmission with idle-boundary preservation, ported from
// c_board.
//
// Two properties matter versus the double-buffer scheme this replaces on mc02.
// The ring is 2048 bytes instead of 128, so a burst of downlink packets no longer
// overflows after two protocol frames. And a packet flagged idle_delimited gets a
// checkpoint recorded at its boundary; try_dequeue refuses to transmit across
// that checkpoint until the line has been idle for 300 us, which is what keeps
// packet framing intact for devices that delimit on idle. The old path simply
// concatenated whatever had accumulated into a single DMA burst and dropped the
// flag on the floor.
class TxBuffer {
public:
    static constexpr size_t kBufferSize = 2048;
    static constexpr size_t kBufferMask = kBufferSize - 1;
    static_assert((kBufferSize & (kBufferSize - 1)) == 0);
    using IndexType = uint16_t;
    static_assert(kBufferSize <= std::numeric_limits<IndexType>::max());

    static constexpr size_t kStagingBufferSize = 1024;
    static_assert(kStagingBufferSize <= std::numeric_limits<IndexType>::max());

    static constexpr size_t kMaxIdleCheckpointCount = 256;
    static_assert((kMaxIdleCheckpointCount & (kMaxIdleCheckpointCount - 1)) == 0);

    explicit TxBuffer(
        UART_HandleTypeDef* hal_uart_handle, void (*dma_complete_callback)(DMA_HandleTypeDef*),
        void (*dma_error_callback)(DMA_HandleTypeDef*))
        : hal_uart_handle_(hal_uart_handle)
        , dma_complete_callback_(dma_complete_callback)
        , dma_error_callback_(dma_error_callback) {
        core::utility::assert_always(hal_uart_handle_ != nullptr);
        // Every port that instantiates this class must have a TX DMA stream.
        // UART5 (DBUS) has none, which is exactly why it uses UartRxOnly.
        core::utility::assert_always(tx_dma_handle() != nullptr);
        bind_tx_dma_callbacks();
    }

    bool try_enqueue(const data::UartDataView& data_view) {
        const auto in = in_.load(std::memory_order::relaxed);
        const auto out = out_.load(std::memory_order::acquire);

        const auto size = data_view.uart_data.size();
        const auto writable = kBufferSize - static_cast<size_t>(static_cast<IndexType>(in - out));
        if (size > writable)
            return false;

        const auto offset = in & kBufferMask;

        if (data_view.idle_delimited) {
            const auto begin_boundary = in;
            const auto end_boundary = static_cast<IndexType>(in + static_cast<IndexType>(size));

            // Optimization: Reuse the logical idle boundary at the current producer position.
            if (idle_boundary_before_in_) {
                if (size) {
                    // Non-empty: Only append the new 'end'.
                    if (!idle_checkpoints_.push_back(end_boundary))
                        return false;
                }
                // If ZLP (size==0): the existing checkpoint already enforces the idle wait.
            } else {
                if (size) {
                    // Non-empty: Push [begin, end] atomically to ensure isolation on both sides.
                    if (idle_checkpoints_.push_back_n(
                            [&, index = 0]() mutable noexcept {
                                return (index++ == 0) ? begin_boundary : end_boundary;
                            },
                            2, true)
                        != 2) {
                        return false;
                    }
                } else {
                    // ZLP: 'begin' == 'end'. Push single checkpoint to force an IDLE wait.
                    if (!idle_checkpoints_.push_back(begin_boundary))
                        return false;
                }
            }
        }

        if (size) {
            const auto slice = std::min(size, kBufferSize - offset);
            const bool wrapped = size != slice;
            if (wrapped)
                trailing_boundary_segmentable_ = !data_view.idle_delimited;

            std::memcpy(ring_buffer_.data() + offset, data_view.uart_data.data(), slice);
            std::memcpy(ring_buffer_.data(), data_view.uart_data.data() + slice, size - slice);

            in_.store(
                static_cast<IndexType>(in + static_cast<IndexType>(size)),
                std::memory_order::release);

            idle_boundary_before_in_ = data_view.idle_delimited;
        } else {
            // Zero-length non-idle packets should not clear an existing boundary.
            idle_boundary_before_in_ |= data_view.idle_delimited;
        }

        return true;
    }

    bool try_dequeue() {
        if (is_busy_.load(std::memory_order::acquire))
            return false;

        // DMA completion only means the last byte reached TDR. On STM32H7 that is
        // not the end of transmission: UART7 runs with FIFO mode enabled
        // (HAL_UARTEx_EnableFifoMode in MX_UART7_Init) and the TXFIFO is 16 deep
        // (DS13313 Table 5), so up to 16 more bytes can still be queued behind
        // it -- roughly 173 us at 921600 baud, the same order as the 300 us idle
        // window this feeds. Timing the window from DMA completion would declare
        // the line idle while it was still transmitting, and the whole point of
        // the checkpoint below is that idle-delimited packets stay separated.
        //
        // ISR.TC is the flag that actually reports "TXFIFO drained and the last
        // stop bit sent"; start_tx_dma() clears TCF before arming, so a set TC
        // here always refers to the transfer that just finished. STM32F407 has
        // no TXFIFO, which is why c_board can time this from DMA completion and
        // still be approximately right.
        if (awaiting_line_completion_ && (hal_uart_handle_->Instance->ISR & USART_ISR_TC) != 0U) {
            awaiting_line_completion_ = false;
            tx_complete_timepoint_ = timer::timer->timepoint();
        }

        // Only the idle window waits on the line draining. Queuing the next chunk
        // behind a partially full TXFIFO is harmless and keeps throughput up, so
        // the dequeue itself is not gated on TC.
        if (!is_idle_ && !awaiting_line_completion_)
            is_idle_ =
                timer::timer->check_expired(tx_complete_timepoint_, std::chrono::microseconds(300));

        core::utility::assert_debug_lazy(
            [&]() noexcept { return (tx_dma_stream()->CR & DMA_SxCR_EN) == 0U; });

        auto out = out_.load(std::memory_order::relaxed);
        if (in_flight_) {
            // For direct ring-buffer DMA, advance out_ only after the previous DMA has finished.
            out = static_cast<IndexType>(out + in_flight_);
            out_.store(out, std::memory_order::release);
            in_flight_ = 0;
        }

        const auto in = in_.load(std::memory_order::acquire);
        const auto readable = static_cast<size_t>(static_cast<IndexType>(in - out));
        if (!readable)
            return false;

        size_t size;
        do {
            size = readable;
            if (auto* idle = idle_checkpoints_.peek_front()) {
                const auto distance = static_cast<size_t>(static_cast<IndexType>(*idle - out));
                core::utility::assert_debug(distance <= readable);
                size = distance;
            }
            size = std::min(size, kStagingBufferSize);

            if (size)
                break;

            // size==0 means out is exactly at a checkpoint boundary.
            // Keep the boundary until the required idle window has elapsed.
            if (!is_idle_)
                return false;

            idle_checkpoints_.pop_front([](const IndexType&) noexcept {});
        } while (true);
        is_idle_ = false;
        is_busy_.store(true, std::memory_order::relaxed);

        const auto offset = out & kBufferMask;
        const auto slice = std::min(size, kBufferSize - offset);
        const bool wrapped = size != slice;

        if (wrapped && !trailing_boundary_segmentable_) {
            // Strict packet must stay contiguous across wrap-around.
            // Flatten into staging and transmit in one DMA shot.
            std::memcpy(staging_buffer_.data(), ring_buffer_.data() + offset, slice);
            std::memcpy(staging_buffer_.data() + slice, ring_buffer_.data(), size - slice);
            out = static_cast<IndexType>(out + static_cast<IndexType>(size));
            out_.store(out, std::memory_order::release);

            start_tx_dma(
                reinterpret_cast<const uint8_t*>(staging_buffer_.data()),
                static_cast<uint16_t>(size));
            return true;
        }

        // Non-strict path can stream directly from ring; commit progress on completion.
        start_tx_dma(
            reinterpret_cast<const uint8_t*>(ring_buffer_.data() + offset),
            static_cast<uint16_t>(slice));
        in_flight_ = static_cast<IndexType>(slice);

        return true;
    }

    void tx_complete_callback() {
        // DMA writes the last byte into the UART TDR, then stop DMA requests from UART.
        ATOMIC_CLEAR_BIT(hal_uart_handle_->Instance->CR3, USART_CR3_DMAT);
        // Provisional stamp; try_dequeue() replaces it with the instant ISR.TC
        // reports the line actually drained. Both writes are published by the
        // release store below and read after the matching acquire load.
        tx_complete_timepoint_ = timer::timer->timepoint();
        awaiting_line_completion_ = true;
        is_busy_.store(false, std::memory_order::release);
    }

    void tx_error_callback() {
        ATOMIC_CLEAR_BIT(hal_uart_handle_->Instance->CR3, USART_CR3_DMAT);
        core::utility::assert_debug_lazy([]() noexcept { return false; });
        tx_complete_timepoint_ = timer::timer->timepoint();
        // The transfer was aborted mid-flight, so TC carries no useful boundary
        // here; fall back to timing the idle window from the abort itself rather
        // than risk waiting on a flag that may describe a partial frame.
        awaiting_line_completion_ = false;
        is_busy_.store(false, std::memory_order::release);
    }

private:
    DMA_HandleTypeDef* tx_dma_handle() const { return hal_uart_handle_->hdmatx; }

    // See the matching comment in rx_buffer.hpp: on STM32H7 the DMA handle's
    // Instance is void*, so register access needs an explicit cast.
    [[nodiscard]] DMA_Stream_TypeDef* tx_dma_stream() const {
        return static_cast<DMA_Stream_TypeDef*>(tx_dma_handle()->Instance);
    }

    void bind_tx_dma_callbacks() {
        auto* dma = tx_dma_handle();
        dma->XferCpltCallback = dma_complete_callback_;
        dma->XferErrorCallback = dma_error_callback_;
        dma->XferHalfCpltCallback = nullptr;
        dma->XferAbortCallback = nullptr;
    }

    void start_tx_dma(const uint8_t* data, uint16_t size) {
        auto* dma = tx_dma_handle();
        bind_tx_dma_callbacks();

        core::utility::assert_always(
            HAL_DMA_Start_IT(
                dma, reinterpret_cast<uint32_t>(data),
                reinterpret_cast<uint32_t>(&hal_uart_handle_->Instance->TDR), size)
            == HAL_OK);

        // STM32H7 clears status through ICR, so the clear takes the ICR bit name
        // rather than the ISR one that F4 uses. Both sit at bit 6, but naming the
        // ICR bit is what actually describes the write.
        __HAL_UART_CLEAR_FLAG(hal_uart_handle_, UART_CLEAR_TCF);
        ATOMIC_SET_BIT(hal_uart_handle_->Instance->CR3, USART_CR3_DMAT);
    }

    UART_HandleTypeDef* hal_uart_handle_;
    void (*dma_complete_callback_)(DMA_HandleTypeDef*);
    void (*dma_error_callback_)(DMA_HandleTypeDef*);

    // Both buffers land in .bss -> AXI SRAM, which MPU region 0 maps as
    // non-cacheable, so the DMA sees these writes without cache maintenance.
    alignas(uint32_t) std::array<std::byte, kBufferSize> ring_buffer_{};
    alignas(uint32_t) std::array<std::byte, kStagingBufferSize> staging_buffer_{};

    std::atomic<IndexType> in_{0};
    std::atomic<IndexType> out_{0};
    static_assert(std::atomic<IndexType>::is_always_lock_free);

    IndexType in_flight_{0};
    std::atomic<bool> is_busy_{false};

    // Producer-only state: whether the current in_ position already has a logical idle
    // boundary associated with it.
    bool idle_boundary_before_in_{false};
    bool trailing_boundary_segmentable_{false};
    bool is_idle_{true};
    // Set by the DMA completion interrupt, cleared by try_dequeue() once ISR.TC
    // confirms the TXFIFO and shift register are empty. See the comment there.
    bool awaiting_line_completion_{false};
    timer::Timer::TimePoint tx_complete_timepoint_{timer::Timer::TimePoint::min()};

    utility::RingBuffer<IndexType, kMaxIdleCheckpointCount> idle_checkpoints_;
};

} // namespace librmcs::firmware::uart
