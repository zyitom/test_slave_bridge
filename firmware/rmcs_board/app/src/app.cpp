#include "firmware/rmcs_board/app/src/app.hpp"

#include <cstdint>

#include <board.h>
#include <device/usbd.h>
#include <hpm_dma_mgr.h>
#include <hpm_l1c_drv.h>

#include "firmware/rmcs_board/app/src/can/can.hpp"
#include "firmware/rmcs_board/app/src/led/led.hpp"
#include "firmware/rmcs_board/app/src/timer/timer.hpp"
#include "firmware/rmcs_board/app/src/uart/uart.hpp"
#include "firmware/rmcs_board/app/src/usb/vendor.hpp"
#include "firmware/rmcs_board/app/src/utility/boot_mailbox.hpp"
#include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"

int main() { librmcs::firmware::app.init().run(); }

namespace librmcs::firmware {

App::App() {
    const utility::InterruptLockGuard guard;

    board_init();
    board_init_usb();
    dma_mgr_init();

    // Enable D-cache write-around: streaming writes bypass cache allocation,
    // keeping the 16 KiB D-cache available for hot control structures.
    l1c_dc_enable_writearound();

    boot::BootMailbox::clear();

    led::led.init();
    timer::timer.init();

    usb::vendor.init();

    for (auto& can : can::can_array)
        can.init();

    for (auto& board_uart : uart::uart_array)
        board_uart.init();
}

// Non-static to ensure instantiation
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
[[noreturn]] void App::run() {
    uint32_t last_tick = 0;
    while (true) {
        tud_task();
        usb::poll_dfu_runtime_reboot();

        // LED bookkeeping runs here at the 1 kHz tick pace instead of inside the
        // mchtmr ISR: MTIP bypasses the PLIC priority threshold, so ISR-side work
        // would preempt even the priority-3 CAN ISR and tax the forwarding hot
        // path. The LED state reflects the session handshake (nonce + keepalive
        // lease), not mere USB enumeration: steady green means data is actually
        // being forwarded; an enumerated host without a live session stays on
        // the "waiting" blink.
        const uint32_t tick = timer::timer->tick_count();
        if (tick != last_tick) {
            last_tick = tick;
            led::led->set_host_connected(usb::vendor->session_established());
            led::led->update(tick);
        }

        usb::vendor->try_transmit();

        for (auto& board_uart : uart::uart_array)
            board_uart->try_transmit();
    }
}

} // namespace librmcs::firmware
