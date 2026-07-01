#include "firmware/mc02/app/src/uart/uart.hpp"

#include <usart.h>

#include "core/src/utility/assert.hpp"

namespace librmcs::firmware::uart {

namespace {

Uart* get_uart_instance(UART_HandleTypeDef* hal_uart_handle) {
    if (hal_uart_handle == &huart1)
        return uart1.get();
    if (hal_uart_handle == &huart7)
        return uart2.get();
    if (hal_uart_handle == &huart10)
        return uart3.get();
    if (hal_uart_handle == &huart5)
        return uart_dbus.get();
    return nullptr;
}

} // namespace

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* hal_uart_handle, uint16_t size) {
    const auto event_type = HAL_UARTEx_GetRxEventType(hal_uart_handle);
    if (event_type == HAL_UART_RXEVENT_HT)
        return;

    auto* uart = get_uart_instance(hal_uart_handle);
    if (!uart)
        return;

    const bool is_idle = (event_type == HAL_UART_RXEVENT_IDLE);
    uart->handle_uplink(size, is_idle);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* hal_uart_handle) {
    auto* uart = get_uart_instance(hal_uart_handle);
    if (!uart)
        return;

    uart->handle_rx_error();
}

} // namespace librmcs::firmware::uart
