#include "firmware/mc02/app/src/uart/uart.hpp"

#include <usart.h>

#include "core/src/utility/assert.hpp"

namespace librmcs::firmware::uart {

namespace {

Uart& get_uart_instance(UART_HandleTypeDef* hal_uart_handle) {
    if (hal_uart_handle == &huart1)
        return *uart1;

    if (hal_uart_handle == &huart7)
        return *uart2;

    if (hal_uart_handle == &huart10)
        return *uart3;

    core::utility::assert_failed_debug();
}

Uart& get_uart_instance_from_dma(DMA_HandleTypeDef* hal_dma_handle) {
    auto* hal_uart_handle = static_cast<UART_HandleTypeDef*>(hal_dma_handle->Parent);
    return get_uart_instance(hal_uart_handle);
}

UartRxOnly& get_dbus_instance_from_dma(DMA_HandleTypeDef* hal_dma_handle) {
    auto* hal_uart_handle = static_cast<UART_HandleTypeDef*>(hal_dma_handle->Parent);
    core::utility::assert_debug(hal_uart_handle == &huart5);
    return *uart_dbus;
}

} // namespace

void Uart::hal_rx_dma_tc_callback(DMA_HandleTypeDef* hal_dma_handle) {
    get_uart_instance_from_dma(hal_dma_handle).rx_dma_tc_callback();
}

void Uart::hal_rx_dma_error_callback(DMA_HandleTypeDef* hal_dma_handle) {
    get_uart_instance_from_dma(hal_dma_handle).rx_dma_error_callback();
}

void Uart::hal_tx_dma_complete_callback(DMA_HandleTypeDef* hal_dma_handle) {
    get_uart_instance_from_dma(hal_dma_handle).tx_complete_callback();
}

void Uart::hal_tx_dma_error_callback(DMA_HandleTypeDef* hal_dma_handle) {
    get_uart_instance_from_dma(hal_dma_handle).tx_dma_error_callback();
}

void UartRxOnly::hal_rx_dma_tc_callback(DMA_HandleTypeDef* hal_dma_handle) {
    get_dbus_instance_from_dma(hal_dma_handle).rx_dma_tc_callback();
}

void UartRxOnly::hal_rx_dma_error_callback(DMA_HandleTypeDef* hal_dma_handle) {
    get_dbus_instance_from_dma(hal_dma_handle).rx_dma_error_callback();
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* hal_uart_handle) {
    if (hal_uart_handle == &huart5) {
        uart_dbus->uart_error_callback();
        return;
    }

    get_uart_instance(hal_uart_handle).uart_error_callback();
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* hal_uart_handle, uint16_t size) {
    (void)size;

    // The HAL raises this for both IDLE and DMA transfer-complete events. With the
    // stream running circular double-buffered, bank completion is handled by the
    // DMA callback instead, so only the IDLE event carries information here.
    if (HAL_UARTEx_GetRxEventType(hal_uart_handle) != HAL_UART_RXEVENT_IDLE)
        return;

    if (hal_uart_handle == &huart5) {
        uart_dbus->rx_event_callback();
        return;
    }

    get_uart_instance(hal_uart_handle).rx_event_callback();
}

} // namespace librmcs::firmware::uart
