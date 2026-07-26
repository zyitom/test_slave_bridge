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
#include "firmware/mc02/app/src/spi/bmi088/service.hpp"
#include "firmware/mc02/app/src/spi/bmi088/temperature.hpp"
#include "firmware/mc02/app/src/timer/timer.hpp"
#include "firmware/mc02/app/src/uart/uart.hpp"
#include "firmware/mc02/app/src/usb/vendor.hpp"
#include "firmware/mc02/app/src/utility/boot_mailbox.hpp"
#include "firmware/mc02/app/src/utility/interrupt_lock.hpp"

int main() {
    SCB->VTOR = 0x08040000U;
    librmcs::firmware::app.init().run();
}

namespace librmcs::firmware {

// Linker-defined bounds of the .itcm section: load image in FLASH (_siitcm) and
// run location in ITCM (_sitcm.._eitcm). extern "C" so they bind to the global
// linker symbols instead of namespaced C++ symbols.
extern "C" {
extern uint32_t _siitcm, _sitcm, _eitcm;
extern uint32_t _sidtcm, _sdtcm, _edtcm;
}

App::App() {
    const utility::InterruptLockGuard guard;

    // Copy the latency-critical CAN forwarding code (the .itcm section) into
    // zero-wait ITCM before anything can call it. app.cpp replaces the CubeMX
    // main(), so this stands in for the startup .data-style copy loop. Done with
    // caches still off, so the writes reach ITCM directly; DSB/ISB then make the
    // new instructions visible to the fetch unit.
    for (uint32_t *dst = &_sitcm, *src = &_siitcm; dst < &_eitcm;)
        *dst++ = *src++;
    // Hot forwarding data (serializer + USB batches + CAN objects) into DTCM, so
    // the uplink ISR touches no AXI bus. Must run before usb/can init below.
    for (uint32_t *dst = &_sdtcm, *src = &_sidtcm; dst < &_edtcm;)
        *dst++ = *src++;
    __DSB();
    __ISB();

    MPU_Config();
    SCB_EnableICache();
    SCB_EnableDCache();
    HAL_Init();
    SystemClock_Config();
    // Enables PLL2 (80 MHz) and selects it as the FDCAN kernel clock for the
    // 1 Mbit/s arbitration + 5 Mbit/s CAN-FD data phase. Generated from the .ioc;
    // must be called here since app.cpp replaces the CubeMX main().
    PeriphCommonClock_Config();

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
    // BDMA is the only DMA controller that reaches SPI6 (D3 domain), so the
    // WS2812 LED depends on it. CubeMX emits its clock enable and NVIC setup in
    // a file of its own, bdma.c, rather than in MX_DMA_Init() -- and app.cpp
    // replaces the generated main(), so the call has to be made here. Must
    // precede MX_SPI6_Init(), whose MspInit runs HAL_DMA_Init() on
    // BDMA_Channel0: with the clock still gated those register writes are
    // dropped, HAL_SPI_Transmit_DMA() then reports HAL_OK for a transfer that
    // never starts, and hspi6 stays BUSY_TX forever with the LED dark.
    MX_BDMA_Init();
    MX_FDCAN1_Init();
    MX_USART10_UART_Init();
    MX_UART7_Init();
    MX_USART1_UART_Init();
    MX_FDCAN2_Init();
    MX_FDCAN3_Init();
    MX_SPI6_Init();
    MX_SPI2_Init();
    MX_UART5_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_TIM5_Init();

    // Start the quarter-microsecond timestamp source before anything that stamps
    // events (UART tx timeouts, IMU data-ready EXTI, SPI uplinks).
    timer::timer.init();

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
    spi::bmi088::accelerometer.init();
    spi::bmi088::gyroscope.init();
    spi::bmi088::temperature.init();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
[[noreturn]] void App::run() {
    while (true) {
        tud_task();
        usb::poll_dfu_runtime_reboot();

        gpio::gpio->poll_periodic_input_samples();

        // Promote a due temperature probe to pending, then service at most one
        // BMI088 SPI read (gyro > accel > temperature) per iteration.
        spi::bmi088::temperature->poll_pending_probe();
        spi::bmi088::service_pending_reads();

        // LED animation; non-blocking unless colour changes (one SPI frame per
        // change, ~330 us at the WS2812 bit rate). Polled here so no ISR context
        // ever touches SPI. The state reported is the host session (nonce
        // handshake plus keepalive lease), not mere USB enumeration, so steady
        // green means data is actually being forwarded.
        led::led->set_host_connected(usb::vendor->session_established());
        led::led->poll();

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
