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
// Reception errors are absorbed rather than recovered from. STM32H7 can keep the
// DMA request alive across a line error (CR3.DDRE) and can stop raising overruns
// altogether (CR3.OVRDIS); see configure_rx_error_policy(). STM32F407 has neither
// bit, so c_board is forced to tear the stream down and restart it every time a
// glitch arrives. Restarting is still implemented here for genuine DMA
// controller faults, and the restart path uses two more registers c_board does
// not have: ICR to drop sticky flags without a destructive RDR read, and
// RQR.RXFRQ to empty the 16-entry RXFIFO.
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
    // Bounds the ISR.REACK poll in wait_receiver_ready(). RE is already
    // acknowledged in steady state, so the loop normally exits on its first
    // read; this only stops restart_rx_dma() from spinning forever inside the
    // DMA error interrupt if the receiver never comes back.
    static constexpr uint32_t kReceiverAckPollLimit = 1024;

    explicit RxBuffer(UART_HandleTypeDef* hal_uart_handle)
        : hal_uart_handle_(hal_uart_handle) {
        configure_rx_error_policy();
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

    // Reception error policy, applied once before any DMA is armed.
    //
    //   CR3.DDRE = 1    a parity/framing/noise error no longer disables the DMA
    //                   request. The suspect byte lands in the ring like any
    //                   other and the protocol layer decides what to do with it,
    //                   instead of one glitch costing the whole 2048-byte ring.
    //   CR3.OVRDIS = 1  overrun detection off. start_rx_dma() deliberately
    //                   leaves CR3.EIE clear, so nothing would ever clear a
    //                   sticky ORE and a set ORE stalls reception. Continuous
    //                   DMA drains RDR fast enough that the flag carried no
    //                   information here anyway.
    //
    // This is also what the mainline Linux driver does for DMA reception:
    // stm32_usart_set_termios() sets USART_CR3_DDRE alongside DMAR whenever an
    // RX channel is present. Neither bit exists on STM32F407.
    //
    // Both are writable only while the USART is disabled, so UE is cycled around
    // the write. TxBuffer's constructor only binds callbacks, so nothing is
    // transmitting or receiving at this point.
    void configure_rx_error_policy() const {
        auto* instance = hal_uart_handle_->Instance;
        const bool was_enabled = (instance->CR1 & USART_CR1_UE) != 0U;

        ATOMIC_CLEAR_BIT(instance->CR1, USART_CR1_UE);
        ATOMIC_SET_BIT(instance->CR3, USART_CR3_DDRE | USART_CR3_OVRDIS);
        if (was_enabled)
            ATOMIC_SET_BIT(instance->CR1, USART_CR1_UE);
    }

    // Drop the sticky RX status through the dedicated clear register, which
    // leaves RDR untouched. On STM32F407 the only way to clear ORE is the "read
    // SR, then read DR" sequence, which would steal a byte from the DMA stream.
    void clear_rx_error_flags() const {
        WRITE_REG(
            hal_uart_handle_->Instance->ICR,
            USART_ICR_PECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_ORECF | USART_ICR_IDLECF);
    }

    // Discard whatever is still held in RDR and in the 16-entry RXFIFO (DS13313
    // Table 5 gives the depth). UART5 and UART7 run with FIFO mode enabled --
    // HAL_UARTEx_EnableFifoMode() in MX_UART5_Init/MX_UART7_Init, while USART1
    // and USART10 disable it -- so without this up to 16 bytes captured before
    // an abort would survive it and be written at ring offset 0 as if they had
    // just arrived. That is the same class of stale-fragment bug that resetting
    // CT in start_rx_dma() prevents. STM32F407 has no request register at all.
    void flush_rx_fifo() const { WRITE_REG(hal_uart_handle_->Instance->RQR, USART_RQR_RXFRQ); }

    // The receiver acknowledges RE asynchronously: it is only sampling the line
    // once ISR.REACK reads back as 1, and STM32F407 has no such handshake. RE is
    // never cleared once CubeMX has enabled it, so this normally exits on the
    // first read.
    void wait_receiver_ready() const {
        auto* instance = hal_uart_handle_->Instance;
        if ((instance->CR1 & USART_CR1_RE) == 0U)
            return;

        for (uint32_t i = 0; i < kReceiverAckPollLimit; ++i) {
            if ((instance->ISR & USART_ISR_REACK) != 0U)
                return;
        }
        core::utility::assert_debug_lazy([]() noexcept { return false; });
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

        // RxXferSize is deliberately one larger than the bank. The IDLE branch of
        // HAL_UART_IRQHandler only reports the event when
        // `0 < __HAL_DMA_GET_COUNTER(hdmarx) < huart->RxXferSize`, and in circular
        // double-buffer mode NDTR reloads to kIrqFragmentSize the moment a bank
        // completes. So a frame whose length is an exact multiple of the bank size
        // and which then goes idle leaves NDTR == kIrqFragmentSize, the comparison
        // fails, HAL_UARTEx_RxEventCallback is never raised, and idle_count never
        // advances -- the consumer would hold that frame until the next fragment
        // arrived. Reporting one byte more than the bank makes the comparison true
        // for every NDTR the hardware can actually present here.
        //
        // The HAL reads RxXferSize nowhere else on this path: UART_DMAReceiveCplt
        // is replaced by hal_rx_dma_tc_callback in bind_rx_dma_callbacks(), and
        // RxXferCount is only written by that IDLE branch, never read by us.
        hal_uart_handle_->RxXferSize = static_cast<uint16_t>(kIrqFragmentSize + 1);
        hal_uart_handle_->RxXferCount = static_cast<uint16_t>(kIrqFragmentSize);

        // Drop every sticky RX flag and every byte still held in RDR/RXFIFO, then
        // confirm the receiver is live, so DMA requests resume on a real byte
        // boundary instead of on leftovers from before the restart.
        clear_rx_error_flags();
        flush_rx_fifo();
        wait_receiver_ready();

        // CR3.EIE and CR1.PEIE stay clear on purpose, and that is what makes
        // CR3.DDRE worth setting at all. HAL_UART_IRQHandler() classifies *any*
        // RX error as blocking whenever DMAR is set -- the condition it tests is
        // `HAL_IS_BIT_SET(huart->Instance->CR3, USART_CR3_DMAR) || ...` -- and
        // responds with UART_EndRxTransfer() followed by HAL_DMA_Abort_IT(). So
        // with the error interrupts enabled the hardware would keep the stream
        // running exactly as DDRE intends and the HAL would abort it anyway,
        // reproducing the F407 behaviour on a part that does not need it.
        //
        // Line errors are absorbed by DDRE/OVRDIS instead. Genuine DMA
        // controller faults still arrive through the stream's own error
        // interrupt (hal_rx_dma_error_callback -> rx_error_callback), which is
        // what restart_rx_dma() now exists for.
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
        // Reached for DMA controller faults (transfer/FIFO/direct-mode errors),
        // not for line errors: with CR3.EIE left clear, a framing/noise/parity
        // glitch no longer raises the UART interrupt at all and DDRE keeps the
        // stream running through it. The HAL error callback in uart.cpp can still
        // route here if the HAL sets an RX ErrorCode by some other path.
        //
        // Deliberately no debug assert, unlike c_board: mc02 drives DBUS and
        // hot-pluggable ports, so the port must recover rather than trap the
        // debug build.
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
