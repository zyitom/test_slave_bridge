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

    // Runtime baudrate switch requested by the host. Writes BRR directly rather
    // than re-running HAL_UART_Init, so every other setting stays exactly as
    // CubeMX generated it and the running DMA is not torn down.
    //
    // This mirrors UART_SetConfig() in the STM32H7 HAL. Unlike the F4 parts,
    // H7 UARTs take their kernel clock from a selectable source (not simply
    // PCLK) and pass it through Init.ClockPrescaler, so the divisor must be
    // computed from HAL_RCCEx_GetPeriphCLKFreq and the prescaler table -- using
    // HAL_RCC_GetPCLKxFreq here would silently produce the wrong baudrate.
    //
    // RX bytes arriving inside the switch window may be garbled; the host is
    // expected to quiesce the link first.
    bool handle_config(const data::UartConfigView& data) {
        if (!data.baudrate.has_value() || *data.baudrate == 0) [[unlikely]]
            return false;

        const uint32_t kernel_clock_hz = peripheral_clock_hz();
        if (kernel_clock_hz == 0U) [[unlikely]]
            return false;

        // Bounds per the HAL: a divisor outside them would misconfigure the
        // peripheral, so reject instead of writing garbage.
        constexpr uint32_t kBrrMin = 0x10U;
        constexpr uint32_t kBrrMax = 0xFFFFU;

        uint32_t brr;
        if (hal_uart_handle_->Init.OverSampling == UART_OVERSAMPLING_8) {
            const uint32_t usartdiv = UART_DIV_SAMPLING8(
                kernel_clock_hz, *data.baudrate, hal_uart_handle_->Init.ClockPrescaler);
            if (usartdiv < kBrrMin || usartdiv > kBrrMax) [[unlikely]]
                return false;
            // Oversampling-by-8 packs BRR[3] as zero and shifts the low nibble.
            brr = (usartdiv & 0xFFF0U) | ((usartdiv & 0x000FU) >> 1U);
        } else {
            const uint32_t usartdiv = UART_DIV_SAMPLING16(
                kernel_clock_hz, *data.baudrate, hal_uart_handle_->Init.ClockPrescaler);
            if (usartdiv < kBrrMin || usartdiv > kBrrMax) [[unlikely]]
                return false;
            brr = usartdiv;
        }

        hal_uart_handle_->Init.BaudRate = *data.baudrate;
        hal_uart_handle_->Instance->BRR = brr;
        return true;
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

    // Called from HAL_UART_ErrorCallback on a UART receive error (PE/NE/FE/ORE)
    // or an RX DMA error. The HAL has already cleared the error flags and aborted
    // the in-flight ReceiveToIdle DMA, leaving RxState == READY; re-arm reception
    // immediately so a corrupted frame costs at most one dropped frame instead of
    // stalling the port until the next try_transmit() poll re-arms it. RX-side
    // only -- a TX error self-heals via the gState check in try_transmit().
    void handle_rx_error() {
        if (hal_uart_handle_->RxState == HAL_UART_STATE_READY) [[likely]]
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

    // Kernel clock feeding this UART's baudrate divider.
    //
    // c_board does the same job in two lines with HAL_RCC_GetPCLKxFreq(), and
    // that is correct there: on STM32F407 a UART is clocked straight off its APB
    // bus, so the bus frequency IS the kernel clock. STM32H7 inserts a
    // selectable clock source per UART group plus Init.ClockPrescaler, so the
    // source has to be resolved before the frequency means anything.
    //
    // Resolved with the HAL's own UART_GETCLOCKSOURCE, which dispatches on the
    // peripheral instance -- the same macro UART_SetConfig() uses to compute BRR
    // at init. Deliberately NOT HAL_RCCEx_GetPeriphCLKFreq(): that function's
    // if/else chain in this HAL version covers SAI/SPI/ADC/SDMMC/FDCAN but has
    // NO branch for either UART group, so RCC_PERIPHCLK_USART16910 and
    // RCC_PERIPHCLK_USART234578 both fall through to its final
    // `else { frequency = 0; }` and return 0. Those macros do exist, so nothing
    // warns at compile time. handle_config() then took its kernel_clock_hz == 0
    // early-out and returned without ever writing BRR: every runtime baudrate
    // request was silently ignored and the port stayed at the CubeMX 115200.
    //
    // Invisible to a same-board UART7<->UART10 loopback: both ends ignored the
    // switch identically, so the loop passed at every requested rate while
    // actually running at 115200 throughout. Only a cross-board test against an
    // end that did switch exposed it.
    [[nodiscard]] uint32_t peripheral_clock_hz() const {
        UART_ClockSourceTypeDef clocksource = UART_CLOCKSOURCE_UNDEFINED;
        UART_GETCLOCKSOURCE(hal_uart_handle_, clocksource);

        switch (clocksource) {
        case UART_CLOCKSOURCE_D2PCLK1: return HAL_RCC_GetPCLK1Freq();
        case UART_CLOCKSOURCE_D2PCLK2: return HAL_RCC_GetPCLK2Freq();
        case UART_CLOCKSOURCE_CSI: return CSI_VALUE;
        case UART_CLOCKSOURCE_LSE: return LSE_VALUE;
        case UART_CLOCKSOURCE_HSI:
            // HSI reaches the UARTs through its divider, so raw HSI_VALUE is
            // only right when that divider is 1. UART_SetConfig shifts the same
            // way. UART7/UART10 select HSI in the .ioc, so this path is live.
            if (__HAL_RCC_GET_FLAG(RCC_FLAG_HSIDIV) != 0U)
                return static_cast<uint32_t>(HSI_VALUE >> (__HAL_RCC_GET_HSI_DIVIDER() >> 3U));
            return static_cast<uint32_t>(HSI_VALUE);
        case UART_CLOCKSOURCE_PLL2: {
            PLL2_ClocksTypeDef pll2{};
            HAL_RCCEx_GetPLL2ClockFreq(&pll2);
            return pll2.PLL2_Q_Frequency;
        }
        case UART_CLOCKSOURCE_PLL3: {
            PLL3_ClocksTypeDef pll3{};
            HAL_RCCEx_GetPLL3ClockFreq(&pll3);
            return pll3.PLL3_Q_Frequency;
        }
        default: return 0;
        }
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
