#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <usart.h>

#include "core/include/librmcs/data/datas.hpp"
#include "core/src/protocol/serializer.hpp"
#include "core/src/utility/assert.hpp"
#include "core/src/utility/immovable.hpp"
#include "firmware/mc02/app/src/led/led.hpp"
#include "firmware/mc02/app/src/uart/rx_buffer.hpp"
#include "firmware/mc02/app/src/uart/tx_buffer.hpp"
#include "firmware/mc02/app/src/usb/helper.hpp"
#include "firmware/mc02/app/src/utility/lazy.hpp"

namespace librmcs::firmware::uart {

// State and behaviour shared by both port flavours: identity, the runtime
// baudrate control surface, and the hand-off of received bytes to USB. It holds
// no buffers of its own, so the RX-only DBUS port does not carry a 3 KB TX ring
// it can never use.
class UartCommon : private core::utility::Immovable {
public:
    // Runtime baudrate switch requested by the host. Writes BRR directly rather
    // than re-running HAL_UART_Init, so every other setting stays exactly as
    // CubeMX generated it and the running DMA is not torn down.
    //
    // This mirrors UART_SetConfig() in the STM32H7 HAL. Unlike the F4 parts,
    // H7 UARTs take their kernel clock from a selectable source (not simply
    // PCLK) and pass it through Init.ClockPrescaler, so the divisor must be
    // computed from the resolved clock source -- using HAL_RCC_GetPCLKxFreq here
    // the way c_board does would silently produce the wrong baudrate.
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

    // The baudrate actually programmed, reconstructed from BRR rather than from
    // Init.BaudRate: the latter is only what was last requested, and on a rejected
    // request neither it nor BRR is written, so reading BRR back is what lets the
    // host tell "switch applied" from "switch refused, old rate still running".
    [[nodiscard]] uint32_t effective_baudrate() const {
        const uint32_t kernel_clock_hz = peripheral_clock_hz();
        const uint32_t brr = hal_uart_handle_->Instance->BRR & 0xFFFFU;
        if (!kernel_clock_hz || !brr) [[unlikely]]
            return 0;
        const uint32_t presc = UARTPrescTable[hal_uart_handle_->Init.ClockPrescaler & 0x0FU];
        const uint32_t clock = presc ? kernel_clock_hz / presc : kernel_clock_hz;
        if (hal_uart_handle_->Init.OverSampling == UART_OVERSAMPLING_8) {
            // Oversampling by 8 stores BRR[3] as zero and the low nibble shifted
            // right by one, so undo that before dividing.
            const uint32_t usartdiv = (brr & 0xFFF0U) | ((brr & 0x0007U) << 1U);
            return usartdiv ? (2U * clock) / usartdiv : 0U;
        }
        return clock / brr;
    }

protected:
    UartCommon(data::DataId data_id, UART_HandleTypeDef* hal_uart_handle)
        : data_id_(data_id)
        , hal_uart_handle_(hal_uart_handle) {}

    // RxBuffer and TxBuffer each keep their own copy of the handle, so the derived
    // ports cannot name hal_uart_handle_ unqualified. Route the two accesses they
    // need through here rather than sprinkling UartCommon:: qualifications.
    [[nodiscard]] bool has_rx_error() const {
        constexpr uint32_t rx_error_mask =
            HAL_UART_ERROR_PE | HAL_UART_ERROR_NE | HAL_UART_ERROR_FE | HAL_UART_ERROR_ORE;
        return (hal_uart_handle_->ErrorCode & rx_error_mask) != 0U;
    }

    void flag_dma_error() { hal_uart_handle_->ErrorCode |= HAL_UART_ERROR_DMA; }

    void handle_uplink(
        std::span<const std::byte> payload, std::span<const std::byte> payload2, bool is_idle) {
        auto& serializer = usb::get_serializer();
        core::utility::assert_always(
            serializer.write_uart(
                data_id_, {.uart_data = payload, .idle_delimited = is_idle}, payload2)
            != core::protocol::Serializer::SerializeResult::kInvalidArgument);
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
};

// Full-duplex port: USART1, UART7 and USART10, each with both an RX and a TX DMA
// stream wired up by CubeMX.
class Uart
    : public UartCommon
    , private TxBuffer
    , private RxBuffer<Uart> {
    friend class RxBuffer<Uart>;

public:
    using Lazy = utility::Lazy<Uart, data::DataId, UART_HandleTypeDef*>;

    Uart(data::DataId data_id, UART_HandleTypeDef* hal_uart_handle)
        : UartCommon(data_id, hal_uart_handle)
        , TxBuffer(hal_uart_handle, &hal_tx_dma_complete_callback, &hal_tx_dma_error_callback)
        , RxBuffer(hal_uart_handle) {}

    void handle_downlink(const data::UartDataView& data) {
        if (!TxBuffer::try_enqueue(data))
            led::led->downlink_buffer_full();
    }

    void try_transmit() {
        RxBuffer::try_dequeue();
        TxBuffer::try_dequeue();
    }

    void tx_complete_callback() { TxBuffer::tx_complete_callback(); }

    void uart_error_callback() {
        if (has_rx_error())
            RxBuffer::rx_error_callback();
    }

    void rx_dma_error_callback() {
        flag_dma_error();
        RxBuffer::rx_error_callback();
    }

    void tx_dma_error_callback() { TxBuffer::tx_error_callback(); }

    void rx_event_callback() { RxBuffer::uart_idle_event_callback(); }

private:
    static void hal_rx_dma_error_callback(DMA_HandleTypeDef* hal_dma_handle);

    static void hal_tx_dma_complete_callback(DMA_HandleTypeDef* hal_dma_handle);

    static void hal_tx_dma_error_callback(DMA_HandleTypeDef* hal_dma_handle);
};

// Receive-only port, used for UART5 (DBUS).
//
// Two independent reasons it carries no TX path: CubeMX wires UART5 with an RX
// DMA stream only (bsp/cubemx/Core/Src/usart.c declares hdma_uart5_rx and no
// hdma_uart5_tx), and the protocol never routes a downlink to kUartDbus anyway --
// usb/vendor.hpp's uart_deserialized_callback dispatches kUart1/kUart2/kUart3 and
// falls through for everything else. Only kUartDbusConfig is routed here, and
// that lands in UartCommon::handle_config.
class UartRxOnly
    : public UartCommon
    , private RxBuffer<UartRxOnly> {
    friend class RxBuffer<UartRxOnly>;

public:
    using Lazy = utility::Lazy<UartRxOnly, data::DataId, UART_HandleTypeDef*>;

    UartRxOnly(data::DataId data_id, UART_HandleTypeDef* hal_uart_handle)
        : UartCommon(data_id, hal_uart_handle)
        , RxBuffer(hal_uart_handle) {}

    // Named to match the full-duplex port so app.cpp can poll every port
    // uniformly; here it only drains the receive ring.
    void try_transmit() { RxBuffer::try_dequeue(); }

    void uart_error_callback() {
        if (has_rx_error())
            RxBuffer::rx_error_callback();
    }

    void rx_dma_error_callback() {
        flag_dma_error();
        RxBuffer::rx_error_callback();
    }

    void rx_event_callback() { RxBuffer::uart_idle_event_callback(); }

private:
    static void hal_rx_dma_error_callback(DMA_HandleTypeDef* hal_dma_handle);
};

// In D2 SRAM (.d2_sram, 0x30000000). Each object carries its own DMA rings, and
// DMA1 -- which drives all four RX streams and the three TX streams -- is a
// D2-domain master: keeping the rings in D2 SRAM keeps every transfer inside the
// domain instead of crossing the D2-to-D1 interconnect to the AXI SRAM and
// contending there with the M7. See the .d2_sram comment in
// bsp/linker/STM32H723VGTx_APP.ld for the second reason (AXI SRAM headroom under
// the non-cacheable MPU window) and for what app.cpp has to do at boot.
//
// Do NOT move these into .dtcm the way can.hpp places its objects: DTCM is
// reachable only by the core, so a DMA stream targeting it silently transfers
// nothing.
[[gnu::section(".d2_sram")]] inline constinit Uart::Lazy uart1{data::DataId::kUart1, &huart1};
[[gnu::section(".d2_sram")]] inline constinit Uart::Lazy uart2{data::DataId::kUart2, &huart7};
[[gnu::section(".d2_sram")]] inline constinit Uart::Lazy uart3{data::DataId::kUart3, &huart10};
[[gnu::section(".d2_sram")]] inline constinit UartRxOnly::Lazy uart_dbus{
    data::DataId::kUartDbus, &huart5};

// RS-485 port on USART2, off by default.
//
// Nothing about the driver differs from the ports above: the transceiver's
// direction pin is driven by the USART itself, not by software. CubeMX wires
// PD4 as USART2_DE and MX_USART2_UART_Init calls HAL_RS485Ex_Init, which sets
// CR3.DEM -- the hardware then asserts DE before the start bit and releases it
// after the stop bit, so the same Uart, the same RxBuffer and the same TxBuffer
// work unchanged. STM32F407's USART has no DEM/DEP at all, which is why c_board
// could never do this without a GPIO and software timing.
//
// Three things still need attention before trusting it on a real bus:
//
//   - Half-duplex echo. A 2-wire bus feeds your own transmission back into your
//     own receiver unless the transceiver's /RE follows DE. The .ioc assigns no
//     RE pin, so the board either ties /RE to DE or uses an auto-direction
//     transceiver -- confirm against the schematic, because if the receiver
//     stays enabled every byte sent is also forwarded upstream as received.
//   - Turnaround. HAL_RS485Ex_Init is generated with DEAT = DEDT = 0, CubeMX's
//     default rather than a decision. RS-485 normally wants a deassertion delay
//     of a sample time or two so the final bit is fully driven before the line
//     is released.
//   - Bus arbitration. TxBuffer's idle checkpoints keep packet boundaries
//     intact, which on a shared bus also has to mean "do not start talking while
//     the peer is answering". The 300 us window is a starting point, not a
//     protocol-correct turnaround time.
//
// Uses DataId::kUart0, the one channel slot mc02 leaves free -- which is also
// what diag/can_diag.cpp and diag/loop_profile.cpp emit on, hence the guard in
// app/CMakeLists.txt making the three mutually exclusive.
#if defined(LIBRMCS_APP_RS485_ENABLE) && LIBRMCS_APP_RS485_ENABLE
[[gnu::section(".d2_sram")]] inline constinit Uart::Lazy uart0{data::DataId::kUart0, &huart2};
#endif

} // namespace librmcs::firmware::uart
