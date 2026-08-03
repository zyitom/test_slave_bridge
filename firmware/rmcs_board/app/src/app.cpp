#include "firmware/rmcs_board/app/src/app.hpp"

#include <cstdint>

#include <board.h>
#include <device/usbd.h>
#include <hpm_dma_mgr.h>
#include <hpm_l1c_drv.h>

#include "firmware/rmcs_board/app/src/can/can.hpp"
#include "firmware/rmcs_board/app/src/diag/can_diag.hpp"
#include "firmware/rmcs_board/app/src/led/led.hpp"
#include "firmware/rmcs_board/app/src/timer/timer.hpp"
#include "firmware/rmcs_board/app/src/uart/uart.hpp"
#include "firmware/rmcs_board/app/src/usb/vendor.hpp"
#include "firmware/rmcs_board/app/src/utility/boot_mailbox.hpp"
#include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"
#include "firmware/rmcs_board/app/src/xcore/flash_server.hpp"
#include "firmware/rmcs_board/app/src/xcore/pd_link.hpp"
#include "firmware/rmcs_board/app/src/xcore/secondary_core.hpp"

int main() { librmcs::firmware::app.init().run(); }

namespace librmcs::firmware {

App::App() {
    {
        const utility::InterruptLockGuard guard;

        board_init();
        board_init_usb();
        dma_mgr_init();

        // Enable D-cache write-around: streaming writes bypass cache allocation,
        // keeping the 16 KiB D-cache available for hot control structures.
        l1c_dc_enable_writearound();

        boot::BootMailbox::clear();

        // Publish the cross-core channel and open the MBX0 clock gate. Both are
        // preconditions for starting core1 and both must follow board_init(),
        // whose board_init_pmp() is what makes SHARE_RAM non-cacheable + AMO --
        // the property the lock-free rings depend on. No-op unless the build
        // enables LIBRMCS_APP_RELEASE_CORE1.
        xcore::publish_channel();

        // Bring up the NOR and arm the cross-core flash RPC. Must precede
        // release_core1(): core1's first act after the channel appears is
        // ecat_flash_eeprom_init(), which may immediately request an erase if the
        // stored SII revision is older than the built-in one. Publishing the
        // geometry late would turn that into a start-up failure rather than a
        // wait, since the client refuses to issue requests to an absent server.
        //
        // core1 delegates because there is one flash on this part and core0 runs
        // from it -- see XcoreFlashRpc in ecat/common/xcore_channel.hpp.
        xcore::flash_server_init();

        led::led.init();
        timer::timer.init();
    }

    // Start core1 with interrupts already restored, and BEFORE USB and the
    // CAN/UART drivers exist.
    //
    // Interrupts must be on: core1 issues a cross-core request within
    // microseconds of release, and a masked core0 would turn that into a
    // nondeterministic start-up stall. Everything core1 waits on (channel magic,
    // MBX0 clock, flash geometry) was published inside the locked section above.
    //
    // Nothing else may be running yet, because core1's first act is
    // ecat_flash_eeprom_init(), which on a revision bump rewrites the emulated
    // SII -- and each sector erase costs this core tens of milliseconds with
    // interrupts masked. With USB enumerated that is long enough to NAK the host
    // out of its session; with the CAN controllers armed it overruns their RX
    // FIFOs. Spending the whole window here, where there is nothing to disturb,
    // is CORE_SWAP_MIGRATION.md section 3.2's requirement, which step 3 shipped
    // without.
    xcore::release_core1();

    // Generous: the worst case is a full SII rewrite, tens of sectors at tens of
    // milliseconds each. Timing out is not fatal -- start-up continues and the
    // only thing lost is the isolation above -- because a core1 that never
    // answers must not take the USB firmware, and with it the DFU path, down.
    xcore::wait_for_core1_eeprom(5000);

    {
        const utility::InterruptLockGuard guard;

        // Before the CAN and UART init() calls below: those arm driver ISRs that
        // serialize straight into the protocol stack, so the stack instance has
        // to exist first.
        //
        // In the core-swap layout the protocol stack instance is xcore::pd_link,
        // not usb::vendor: link::uplink_serializer() resolves to the PD stream
        // (xcore/pd_link.cpp) because migration step 1 hard-codes "EtherCAT always
        // owns the data plane". usb::vendor is still constructed -- it is what
        // brings up TinyUSB, so enumeration and DFU-RT (the flashing path) keep
        // working -- but its data plane is detached; see app/CMakeLists.txt.
        xcore::pd_link_init();
        usb::vendor.init();

        for (auto& can : can::can_array)
            can.init();

        for (auto& board_uart : uart::uart_array)
            board_uart.init();
    }
}

namespace {

// LED source: the transport that actually owns the data plane in this build.
// Steady green must mean "frames are being forwarded", so it has to follow the
// session that the CAN/UART drivers serialize into -- reporting USB enumeration
// while EtherCAT carries the traffic would make the indicator lie.
bool host_session_established() {
#if defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1
    return xcore::pd_link->session_established();
#else
    return usb::vendor->session_established();
#endif
}

} // namespace

// Non-static to ensure instantiation
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
[[noreturn]] void App::run() {
    uint32_t last_tick = 0;
    while (true) {
        diag::note_main_loop();
        tud_task();

        // Drain the CAN software transmit queues immediately after tud_task(),
        // which is where TinyUSB delivers this pass's downlink frames. Placing
        // it later -- after the 1 kHz LED/telemetry block -- would put that
        // block's work between a frame arriving and reaching the wire.
        for (auto& can : can::can_array)
            can->try_transmit();

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
            led::led->set_host_connected(host_session_established());
            led::led->update(tick);

            // CAN forwarding telemetry (LIBRMCS_APP_CAN_DIAG builds only).
            // Paced off the same 1 kHz tick and emitted before the transport
            // pump below, so a record produced this tick leaves on this pass.
            diag::poll(tick);
        }

        // CAN interrupt-delivery watchdog. Must run every pass, not off the
        // 1 kHz tick: RX FIFO0 holds 32 elements, which at the rates this board
        // forwards is under two milliseconds of slack before frames are lost.
        for (auto& can : can::can_array)
            can->poll();

        // Host transport pump.
        //
        // In the core-swap layout xcore::pump_data_plane() drives BOTH backends
        // of the single merged session (USB and the cross-core PD stream, picked
        // by the arbitration owner), so usb::vendor holds no session of its own
        // and its next_batch() returns nullptr on every pass. It stays in the
        // loop only to keep single-core builds -- where usb::Vendor IS the host
        // transport -- on the identical code path.
        usb::vendor->try_transmit();
        xcore::pump_data_plane();

        for (auto& board_uart : uart::uart_array)
            board_uart->try_transmit();

        // core1's only log path: it must not printf (ecat/common/xcore_diag.hpp
        // explains why a single printf there corrupts core0's vector table), so
        // core0 relays its diagnostic ring to the console. A two-load no-op when
        // the ring is empty, and compiled out entirely in single-core builds.
        xcore::poll_diagnostics();
    }
}

} // namespace librmcs::firmware
