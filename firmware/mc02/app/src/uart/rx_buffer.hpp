#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <main.h>
#include <usart.h>

#include "core/src/protocol/constant.hpp"
#include "core/src/protocol/protocol.hpp"
#include "core/src/utility/assert.hpp"
#include "firmware/mc02/app/src/utility/interrupt_lock.hpp"

namespace librmcs::firmware::uart {

// Continuous DMA reception into a power-of-two ring, ported from c_board.
//
// The DMA runs in double-buffer mode over kIrqFragmentSize-sized banks and is
// never stopped: on every bank completion only the idle bank's target address is
// advanced, so USART_CR3_DMAR stays set for the entire life of the port. That is
// the whole point of this class. The previous mc02 implementation used
// HAL_UARTEx_ReceiveToIdle_DMA with a one-shot (DMA_NORMAL) stream, and this HAL
// clears DMAR and aborts the stream *before* invoking the RX event callback --
// see UART_DMAReceiveCplt() and the IDLE branch of HAL_UART_IRQHandler in
// bsp/stm32h7xx-hal-driver/Src/stm32h7xx_hal_uart.c. Reception was therefore off
// from that abort until the application re-armed it, and because the one-shot
// buffer was 64 bytes, any stream longer than 64 bytes hit that window once per
// 64 bytes and lost whatever arrived inside it.
//
// The consumer runs in the main loop (try_dequeue), so the interrupt path only
// advances an index and never touches USB.
template <typename T>
class RxBuffer {
    friend T;

public:
    static constexpr size_t kBufferSize = 2048;
    static constexpr size_t kBufferMask = kBufferSize - 1;
    static_assert((kBufferSize & (kBufferSize - 1)) == 0);
    using IndexType = uint16_t;
    static_assert(kBufferSize <= std::numeric_limits<IndexType>::max());

    static constexpr size_t kIrqFragmentSize = 32;
    static constexpr size_t kRxSlotCount = kBufferSize / kIrqFragmentSize;
    static_assert(kBufferSize % kIrqFragmentSize == 0);
    static_assert((kRxSlotCount & (kRxSlotCount - 1)) == 0);

    static constexpr size_t kMinFragmentSize = 32;
    static constexpr size_t kMaxFragmentSize = kMinFragmentSize + kIrqFragmentSize - 1;
    static constexpr size_t kProtocolMaxPayloadSize =
        core::protocol::kProtocolBufferSize - sizeof(core::protocol::UartHeaderExtended);
    static_assert(0 < kMinFragmentSize && kMaxFragmentSize <= kProtocolMaxPayloadSize);

    bool try_dequeue() {
        auto state = in_state_.load(std::memory_order::acquire);
        const auto out = out_.load(std::memory_order::relaxed);
        const auto readable = static_cast<size_t>(static_cast<IndexType>(state.in - out));

        if (readable > kBufferSize) [[unlikely]] {
            // Abnormal condition: The circular queue has wrapped around.
            // Fail-fast in debug builds to catch timing/interrupt issues early.
            core::utility::assert_debug_lazy([]() noexcept { return false; });

            // Release fallback: discard the accumulated bytes to resync the stream.
            out_.store(state.in, std::memory_order::release);
            consumed_idle_count_ = state.idle_count;
            return false;
        }

        const bool is_idle = (readable <= kProtocolMaxPayloadSize)
                               ? (state.idle_count != consumed_idle_count_)
                               : false;
        if (is_idle)
            consumed_idle_count_ = state.idle_count;
        else if (readable < kMinFragmentSize)
            return false;

        const auto size = std::min(readable, kProtocolMaxPayloadSize);
        const auto offset = out & kBufferMask;
        const auto first_size = std::min(size, kBufferSize - offset);
        const auto second_size = size - first_size;

        static_cast<T*>(this)->handle_uplink(
            {ring_.data() + offset, first_size}, {ring_.data(), second_size}, is_idle);

        out_.store(
            static_cast<IndexType>(out + static_cast<IndexType>(size)), std::memory_order::release);
        return true;
    }

private:
    explicit RxBuffer(UART_HandleTypeDef* hal_uart_handle)
        : hal_uart_handle_(hal_uart_handle) {
        bind_rx_dma_callbacks();
        start_rx_dma();
    }

    // STM32H7 declares DMA_HandleTypeDef::Instance as void* (F4 types it as
    // DMA_Stream_TypeDef*), so every register access has to go through this cast.
    // All four UART RX streams are DMA1 streams, never BDMA -- BDMA only serves
    // the D3 domain, and UART5/UART7/USART1/USART10 all live in D2.
    [[nodiscard]] DMA_Stream_TypeDef* rx_dma_stream() const {
        return static_cast<DMA_Stream_TypeDef*>(hal_uart_handle_->hdmarx->Instance);
    }

    void bind_rx_dma_callbacks() {
        auto* hal_dma_handle = hal_uart_handle_->hdmarx;
        core::utility::assert_debug(hal_dma_handle != nullptr);

        hal_dma_handle->XferCpltCallback = &T::hal_rx_dma_tc_callback;
        hal_dma_handle->XferM1CpltCallback = &T::hal_rx_dma_tc_callback;
        hal_dma_handle->XferErrorCallback = &T::hal_rx_dma_error_callback;
        hal_dma_handle->XferHalfCpltCallback = nullptr;
        hal_dma_handle->XferM1HalfCpltCallback = nullptr;
        hal_dma_handle->XferAbortCallback = nullptr;
    }

    void start_rx_dma() {
        auto* hal_dma_handle = hal_uart_handle_->hdmarx;
        core::utility::assert_debug(hal_dma_handle->Init.Mode == DMA_CIRCULAR);

        auto in_state = in_state_.load(std::memory_order::relaxed);
        in_state.in = 0;
        in_state_.store(in_state, std::memory_order::relaxed);
        out_.store(0, std::memory_order::relaxed);

        // Force the stream back onto bank 0 so it agrees with the M0AR/M1AR pair
        // programmed below. Nothing else does this: HAL_DMA_Abort() leaves CT
        // untouched and HAL_DMAEx_MultiBufferStart_IT() never writes it either, so
        // a restart after an RX error could resume on M1AR while the bookkeeping in
        // update_in_and_switch_bank_if_requested() assumes ring offset 0 -- which
        // would publish one stale kIrqFragmentSize fragment before resynchronizing.
        // CT is writable only while EN is low; the caller guarantees that.
        core::utility::assert_debug((rx_dma_stream()->CR & DMA_SxCR_EN) == 0U);
        ATOMIC_CLEAR_BIT(rx_dma_stream()->CR, DMA_SxCR_CT);

        const auto source =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hal_uart_handle_->Instance->RDR));
        const auto destination_0 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ring_.data()));
        const auto destination_1 =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ring_.data() + kIrqFragmentSize));

        core::utility::assert_always(
            HAL_DMAEx_MultiBufferStart_IT(
                hal_dma_handle, source, destination_0, destination_1, kIrqFragmentSize)
            == HAL_OK);

        // Present the port to the HAL as an in-progress circular ReceiveToIdle so
        // that HAL_UART_IRQHandler takes its DMA_CIRCULAR branch on IDLE: it then
        // reports the event through HAL_UARTEx_RxEventCallback without touching
        // DMAR or aborting the stream.
        hal_uart_handle_->RxState = HAL_UART_STATE_BUSY_RX;
        hal_uart_handle_->ReceptionType = HAL_UART_RECEPTION_TOIDLE;
        hal_uart_handle_->RxXferSize = static_cast<uint16_t>(kIrqFragmentSize);
        hal_uart_handle_->RxXferCount = static_cast<uint16_t>(kIrqFragmentSize);

        // Clear stale RX/IDLE flags before enabling DMAR to avoid consuming fresh bytes
        // through ISR/RDR reads while DMA reception is active.
        __HAL_UART_CLEAR_IDLEFLAG(hal_uart_handle_);
        ATOMIC_SET_BIT(hal_uart_handle_->Instance->CR3, USART_CR3_EIE);
        if (hal_uart_handle_->Init.Parity != UART_PARITY_NONE)
            ATOMIC_SET_BIT(hal_uart_handle_->Instance->CR1, USART_CR1_PEIE);
        ATOMIC_SET_BIT(hal_uart_handle_->Instance->CR3, USART_CR3_DMAR);
        ATOMIC_SET_BIT(hal_uart_handle_->Instance->CR1, USART_CR1_IDLEIE);
    }

    void uart_idle_event_callback() {
        // Every UART and every DMA stream on this board is at NVIC priority 3
        // (bsp/cubemx/Core/Src/usart.c and dma.c), so the UART and DMA handlers
        // cannot preempt each other and the guard below only has to shut out
        // higher-priority sources.
        update_in_and_switch_bank_if_requested(true, false);
    }

    void dma_tc_callback() { update_in_and_switch_bank_if_requested(false, true); }

    void rx_error_callback() {
        // Deliberately no debug assert here, unlike c_board: mc02 drives DBUS and
        // hot-pluggable ports where a framing/overrun glitch is a normal event, and
        // the port must recover instead of trapping the debug build. One corrupted
        // frame is the whole cost.
        restart_rx_dma();
    }

    void restart_rx_dma() {
        auto* hal_dma_handle = hal_uart_handle_->hdmarx;

        ATOMIC_CLEAR_BIT(hal_uart_handle_->Instance->CR1, USART_CR1_IDLEIE);
        ATOMIC_CLEAR_BIT(hal_uart_handle_->Instance->CR3, USART_CR3_DMAR);

        if ((rx_dma_stream()->CR & DMA_SxCR_EN) != 0U) {
            core::utility::assert_always(HAL_DMA_Abort(hal_dma_handle) == HAL_OK);
        }

        // The HAL's blocking-error path installs UART_DMAAbortOnError as the
        // stream's abort callback; re-bind so a later HAL_DMA_Abort cannot bounce
        // back into the HAL error machinery behind our back.
        bind_rx_dma_callbacks();
        start_rx_dma();
    }

    void update_in_and_switch_bank_if_requested(bool is_idle, bool switch_bank) {
        const utility::InterruptLockGuard guard;

        auto* hal_dma_instance = rx_dma_stream();

        const uint8_t active_bank =
            static_cast<uint8_t>((hal_dma_instance->CR & DMA_SxCR_CT) >> DMA_SxCR_CT_Pos);
        const auto remaining_count = static_cast<size_t>(hal_dma_instance->NDTR);

        const uintptr_t ring_base = reinterpret_cast<uintptr_t>(ring_.data());
        const uintptr_t active_target_base = active_bank == 0
                                               ? static_cast<uintptr_t>(hal_dma_instance->M0AR)
                                               : static_cast<uintptr_t>(hal_dma_instance->M1AR);
        core::utility::assert_debug(
            ring_base <= active_target_base
            && active_target_base <= ring_base + (kBufferSize - kIrqFragmentSize));

        const auto active_base_offset = static_cast<size_t>(active_target_base - ring_base);
        core::utility::assert_debug(active_base_offset % kIrqFragmentSize == 0);

        if (switch_bank) {
            const auto next_base_offset = (active_base_offset + kIrqFragmentSize) & kBufferMask;
            (active_bank == 0 ? hal_dma_instance->M1AR : hal_dma_instance->M0AR) =
                static_cast<uint32_t>(ring_base + next_base_offset);
        }

        core::utility::assert_debug(remaining_count <= kIrqFragmentSize);
        const auto bank_produced = kIrqFragmentSize - remaining_count;
        const auto writing_offset = active_base_offset + bank_produced;
        core::utility::assert_debug(writing_offset <= kBufferSize);

        auto state = in_state_.load(std::memory_order::relaxed);
        const auto masked_writing_offset = static_cast<IndexType>(writing_offset & kBufferMask);

        auto new_in = static_cast<IndexType>((state.in & ~kBufferMask) | masked_writing_offset);
        if (new_in < state.in)
            new_in = static_cast<IndexType>(new_in + static_cast<IndexType>(kBufferSize));
        core::utility::assert_debug(
            static_cast<size_t>(static_cast<IndexType>(new_in - state.in))
            < (2 * kIrqFragmentSize));

        state.in = new_in;
        state.idle_count += is_idle;
        in_state_.store(state, std::memory_order::release);
    }

    UART_HandleTypeDef* hal_uart_handle_;

    // Lives in .bss, which the linker script maps to AXI SRAM at 0x24000000. MPU
    // region 0 covers that whole 128 KB as non-cacheable (Core/Src/main.c
    // MPU_Config), so DMA writes need no cache maintenance. Do not move this into
    // .dtcm the way can.hpp places its objects -- DMA1/DMA2 cannot reach DTCM.
    alignas(uint32_t) std::array<std::byte, kBufferSize> ring_{};

    struct alignas(uint32_t) InState {
        IndexType in;
        uint16_t idle_count;
    };
    static_assert(sizeof(InState) == sizeof(uint32_t));

    std::atomic<InState> in_state_{
        {.in = 0, .idle_count = 0},
    };
    uint16_t consumed_idle_count_{0};

    std::atomic<IndexType> out_{0};

    static_assert(std::atomic<InState>::is_always_lock_free);
    static_assert(std::atomic<IndexType>::is_always_lock_free);
};

} // namespace librmcs::firmware::uart
