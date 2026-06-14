#include "firmware/mc02/app/src/app.hpp"

#include <bdma.h>
#include <device/usbd.h>
#include <dma.h>
#include <fdcan.h>
#include <gpio.h>
#include <main.h>
#include <spi.h>
#include <tim.h>
#include <usart.h>

#include "firmware/mc02/app/src/can/can.hpp"
#include "firmware/mc02/app/src/gpio/gpio.hpp"
#include "firmware/mc02/app/src/led/led.hpp"
#include "firmware/mc02/app/src/spi/bmi088/accel.hpp"
#include "firmware/mc02/app/src/spi/bmi088/gyro.hpp"
#include "firmware/mc02/app/src/uart/uart.hpp"
#include "firmware/mc02/app/src/usb/vendor.hpp"
#include "firmware/mc02/app/src/utility/boot_mailbox.hpp"
#include "firmware/mc02/app/src/utility/interrupt_lock.hpp"

int main() {
    SCB->VTOR = 0x08040000U;
    librmcs::firmware::app.init().run();
}

namespace librmcs::firmware {

App::App() {
    const utility::InterruptLockGuard guard;

    MPU_Config();
    SCB_EnableICache();
    SCB_EnableDCache();
    HAL_Init();
    SystemClock_Config();

    utility::boot_mailbox.clear();

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    RCC_PeriphCLKInitTypeDef usb_clk = {};
    usb_clk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    usb_clk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&usb_clk) != HAL_OK)
        Error_Handler();
    HAL_PWREx_EnableUSBVoltageDetector();
    __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_BDMA_Init();
    MX_FDCAN1_Init();
    MX_USART10_UART_Init();
    MX_UART7_Init();
    MX_USART1_UART_Init();
    MX_FDCAN2_Init();
    MX_FDCAN3_Init();
    MX_SPI6_Init();
    // MX_SPI2_Init();
    MX_UART5_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();

    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

    led::led.init();
    gpio::gpio.init();
    usb::vendor.init();
    can::can1.init();
    can::can2.init();
    can::can3.init();
    uart::uart1.init();
    uart::uart2.init();
    uart::uart3.init();
    uart::uart_dbus.init();
    // spi::bmi088::accelerometer.init();
    // spi::bmi088::gyroscope.init();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
[[noreturn]] void App::run() {
    while (true) {
        tud_task();

        if (usb::dfu_reboot_pending()) {
            for (int i = 0; i < 1000; ++i)
                tud_task();
            usb::perform_dfu_reboot();
        }

        usb::vendor->try_transmit();
        can::can1->try_transmit();
        usb::vendor->try_transmit();
        can::can2->try_transmit();
        usb::vendor->try_transmit();
        can::can3->try_transmit();
        usb::vendor->try_transmit();
        uart::uart1->try_transmit();
        usb::vendor->try_transmit();
        uart::uart2->try_transmit();
        usb::vendor->try_transmit();
        uart::uart3->try_transmit();
        usb::vendor->try_transmit();
        uart::uart_dbus->try_transmit();
    }
}

} // namespace librmcs::firmware
