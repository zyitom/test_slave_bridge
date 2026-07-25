#include "firmware/ch32_board/app/src/app.hpp"

extern "C" {
#include "ch32h417.h"
#include "ch32h417_usbss_device.h"
#include "debug.h"
#include "usb_desc.h"
}

#include "firmware/ch32_board/app/src/can/can.hpp"
#include "firmware/ch32_board/app/src/led/led.hpp"
#include "firmware/ch32_board/app/src/timer/timer.hpp"
#include "firmware/ch32_board/app/src/uart/uart.hpp"
#include "firmware/ch32_board/app/src/usb/dfu_runtime.hpp"
#include "firmware/ch32_board/app/src/usb/vendor.hpp"

#include "firmware/ch32_board/boot/src/mailbox.hpp"

int main() { librmcs::firmware::app.init().run(); }

namespace librmcs::firmware {
namespace {

// Bring-up instrumentation for the USB 3.0 link state machine. The LTSSM state
// cannot be read with a debugger: halting resets the peripherals (see
// PITFALLS.md 4.4), so the only way to observe it is to have the firmware record
// it into RAM that survives a reset. Same 0x20170000 diag convention the V3F boot
// core uses, continuing past the words it owns (diag[0..10]).
//
// diag[11] bitmask of every LTSSM state seen, bit N = LINK_STATE value N
//          (0=U0 1=U1 2=U2 3=U3 4=DISABLE 5=RXDET 6=INACTIVE 7=POLLING
//           8=RECOVERY 9=HOTRST A=COMPLIANCE B=LOOPBACK)
// diag[12] last raw LINK_STATUS
// diag[13] number of LTSSM state transitions observed
// diag[14] USBSS_DevEnumStatus (non-zero once the host has configured us)
//
// TODO(usb-bringup): drop this once the SS link is understood and stable.
void poll_usb_link_diagnostics() {
    constexpr uintptr_t kBootDiagAddr = 0x20170000u;
    auto* diag = reinterpret_cast<volatile uint32_t*>(kBootDiagAddr);

    static uint32_t previous_state = 0xFFFFFFFFu;

    const uint32_t status = USBSSD->LINK_STATUS;
    const uint32_t state = (status & LINK_STATE_MASK) >> 8u;

    diag[11] |= 1u << state;
    diag[12] = status;
    if (state != previous_state) {
        previous_state = state;
        diag[13]++;
    }
    diag[14] = USBSS_DevEnumStatus;
}

uint32_t delay_hclk_clock() {
    if (HCLKClock != 0)
        return HCLKClock;

    const uint32_t fpre_shift[] = {0, 1, 2, 2};
    const uint32_t fpre = (RCC->CFGR0 & RCC_FPRE) >> 16;
    const uint32_t fallback = SystemCoreClock >> fpre_shift[fpre];
    return fallback != 0 ? fallback : HSI_VALUE;
}

} // namespace

App::App() {
    SystemAndCoreClockUpdate();
    HCLKClock = delay_hclk_clock();
    Delay_Init();
    USART_Printf_Init(921600);
    Chip = ((DBGMCU_GetCHIPID() >> 4) & 0x0F);

    // Free-running timestamp source first: everything below may stamp events.
    timer::timer.init();

    led::led.init();

    // Every consumer the USB interrupt path touches must exist BEFORE the USBSS
    // interrupts are enabled: USBSS_Device_Init() arms the link state machine and
    // NVIC_EnableIRQ, and the very first LINK/EP interrupt reaches into
    // usb::vendor (and through handle_downlink into can/uart). Constructing them
    // afterwards races an already-live ISR against an uninitialised Lazy.
    usb::vendor.init();

    for (auto& can : can::can_array)
        can.init();
    for (auto& board_uart : uart::uart_array)
        board_uart.init();

    // USB 3.0 SuperSpeed device bring-up (WCH USBSS controller), last. Once
    // USB_Timer_Init has armed the link-training helper timer and
    // USBSS_Device_Init has enabled the PHY, link and endpoints, the bulk
    // endpoints are the librmcs uplink/downlink and interrupts are live. The
    // string descriptors are built first: the host reads them during the
    // enumeration that USBSS_Device_Init kicks off.
    librmcs_usb_init_descriptors();
    USB_Timer_Init();

    // The link-diagnostic accumulators live outside every linked section, so
    // nothing zeroes them on reset -- do it before the first USB interrupt can
    // fire. See poll_usb_link_diagnostics().
    {
        auto* diag = reinterpret_cast<volatile uint32_t*>(0x20170000u);
        for (int i = 11; i <= 25; i++)
            diag[i] = 0;
    }

    USBSS_Device_Init(ENABLE);

    // Dual-core handshake: this V5F core is the forwarding fast path. Signal the
    // V3F boot/offload core that bring-up is complete; V3F spins on this before
    // entering its offload loop. The mailbox itself is constructed by V3F before
    // it wakes us, so it is already valid here. See boot/src/main.cpp.
    boot::shared().v5f_ready = 1;
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

        // Drain V3F offload telemetry off the hot path: non-blocking (a single
        // acquire-load when empty), so it never stalls forwarding. This is the
        // cross-core mailbox consumer end.
        // TODO(usb-bringup): route these records into the USB SS uplink once the
        // SS data path is live and a telemetry DataId channel exists; for now
        // they are drained (and would be stamped via timer::timer on use).
        boot::shared().telemetry.pop_front_n(
            [](boot::TelemetryRecord&& record) noexcept { (void)record; });

        // Host asked us to reboot into DFU (see usb/dfu_runtime.cpp); handled
        // here rather than in the ISR so the control transfer completes first.
        usb::poll_dfu_runtime_reboot();

        poll_usb_link_diagnostics();
    }
}

} // namespace librmcs::firmware
