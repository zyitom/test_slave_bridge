#include "firmware/ch32_board/app/src/app.hpp"

extern "C" {
#include "ch32h417.h"
#include "ch32h417_usbss_device.h"
#include "debug.h"
}

#include "firmware/ch32_board/app/src/can/can.hpp"
#include "firmware/ch32_board/app/src/led/led.hpp"
#include "firmware/ch32_board/app/src/timer/timer.hpp"
#include "firmware/ch32_board/app/src/uart/uart.hpp"
#include "firmware/ch32_board/app/src/usb/vendor.hpp"

int main() { librmcs::firmware::app.init().run(); }

namespace librmcs::firmware {

App::App() {
    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(921600);

    // Free-running timestamp source first: everything below may stamp events.
    timer::timer.init();

    // USB 3.0 SuperSpeed device bring-up (WCH USBSS controller). USB_Timer_Init
    // arms the link-training helper timer; USBSS_Device_Init enables the PHY,
    // link and endpoints. The bulk endpoints become the librmcs uplink/downlink.
    USB_Timer_Init();
    USBSS_Device_Init(ENABLE);

    led::led.init();
    usb::vendor.init();

    for (auto& can : can::can_array)
        can.init();
    for (auto& board_uart : uart::uart_array)
        board_uart.init();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
[[noreturn]] void App::run() {
    while (true) {
        // Same shape as upstream rmcs_board's run loop. CAN has no try_transmit:
        // downlink writes straight to the hardware TX mailboxes in
        // handle_downlink. The WCH USBSS and CAN/USART receive paths are all
        // interrupt-driven, so no polled USB task is needed (unlike TinyUSB's
        // tud_task()).
        usb::vendor->try_transmit();

        for (auto& board_uart : uart::uart_array)
            board_uart->try_transmit();
    }
}

} // namespace librmcs::firmware
