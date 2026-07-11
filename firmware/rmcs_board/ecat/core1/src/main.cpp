#include <board.h>

#if defined(RMCS_ECAT_CORE1_CAN_PIN_SCANNER) && RMCS_ECAT_CORE1_CAN_PIN_SCANNER

namespace librmcs::firmware::board {
int can_pin_scanner_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::can_pin_scanner_main();
}

#elif defined(RMCS_ECAT_CORE1_LED_PIN_SCANNER) && RMCS_ECAT_CORE1_LED_PIN_SCANNER

namespace librmcs::firmware::board {
int led_pin_scanner_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::led_pin_scanner_main();
}

#elif defined(RMCS_ECAT_CORE1_LED_CONFIRM) && RMCS_ECAT_CORE1_LED_CONFIRM

namespace librmcs::firmware::board {
int led_confirm_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::led_confirm_main();
}

#elif defined(RMCS_ECAT_CORE1_MDIO_PIN_SCANNER) && RMCS_ECAT_CORE1_MDIO_PIN_SCANNER

namespace librmcs::firmware::board {
int mdio_pin_scanner_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::mdio_pin_scanner_main();
}

#elif defined(RMCS_ECAT_CORE1_ENET_PACKET_TESTER) && RMCS_ECAT_CORE1_ENET_PACKET_TESTER

namespace librmcs::firmware::board {
int enet_packet_tester_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::enet_packet_tester_main();
}

#elif defined(RMCS_ECAT_CORE1_REALTEK_RESET_SCANNER) && RMCS_ECAT_CORE1_REALTEK_RESET_SCANNER

namespace librmcs::firmware::board {
int realtek_reset_scanner_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::realtek_reset_scanner_main();
}

#elif defined(RMCS_ECAT_CORE1_ECAT_STATUS_PROBE) && RMCS_ECAT_CORE1_ECAT_STATUS_PROBE

namespace librmcs::firmware::board {
int ecat_status_probe_main();
}

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    return librmcs::firmware::board::ecat_status_probe_main();
}

#else

# include <atomic>
# include <cstddef>
# include <cstdint>
# include <span>

# include <hpm_mbx_drv.h>

# include "xcore_channel.hpp"

namespace {

// Cross-core uplink doorbell (core1 -> core0). After core1 publishes a
// telemetry batch into the up ring, it pokes HPM_MBX0B; core0 takes the
// matching HPM_MBX0A interrupt and maps the reply into the ESC input image at
// once, instead of waiting for the next SSC MainLoop pass -- collapsing the
// produce-to-ESC turnaround from that loop's slow-path granularity (tens of
// us) to interrupt latency. See ecat_appl.c (rmcs_uplink_doorbell_isr) and
// ../README.md ("uplink refresh").
//
// core0 owns the shared MBX0 clock (rmcs_uplink_doorbell_init runs before this
// core is released), so here only the HPM_MBX0B port needs resetting.
void uplink_doorbell_init() { mbx_init(HPM_MBX0B); }

void uplink_doorbell_ring() {
    // Publish the ring bytes BEFORE the doorbell. XcoreRing::try_push ends in a
    // release store, which orders the payload before the index but does NOT
    // order that non-cacheable store ahead of the following device-register
    // write on RISC-V. A full fence (memory + I/O, both directions) guarantees
    // core0 observes the pushed bytes once it sees the mailbox word.
    __asm__ volatile("fence" ::: "memory");
    // Send failure means a poke is already pending in the single-word mailbox;
    // ignore it -- one interrupt is enough, and the up ring is the source of
    // truth the handler re-reads.
    (void)mbx_send_message(HPM_MBX0B, 0);
}

} // namespace

// Core1: the fieldbus core of the EtherCAT stream bridge.
//
// Default build: the librmcs protocol application -- deserialize the host
// command stream from the down ring into the CAN/UART drivers, serialize
// their uplink back into the up ring, with the same session handshake as the
// USB firmware (see host_link.hpp).
//
// -DRMCS_ECAT_CORE1_LOOPBACK=1 restores the P1 bring-up image: a lossless
// byte echo with zero peripheral code, which the host tool
// ecat_stream_latency uses for byte-exact link validation and RTT scans.

# if defined(RMCS_ECAT_CORE1_LOOPBACK) && RMCS_ECAT_CORE1_LOOPBACK

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
    uplink_doorbell_init();

    auto& channel = librmcs::firmware::ecat::xcore_channel_wait();

    std::byte buffer[256];
    while (true) {
        const std::size_t received = channel.down.pop(buffer);
        if (received == 0)
            continue;

        const std::span<const std::byte> chunk{buffer, received};
        // Spin until the uplink ring has room: the echo must be lossless for
        // the end-to-end ARQ test to be meaningful.
        while (!channel.up.try_push(chunk)) {}
        // Wake core0 to publish the echo now, so the RTT scan measures the
        // doorbell path rather than the MainLoop poll granularity.
        uplink_doorbell_ring();
    }

    return 0;
}

# else

#  include <hpm_dma_mgr.h>
#  include <hpm_l1c_drv.h>

#  include "firmware/rmcs_board/app/src/can/can.hpp"
#  include "firmware/rmcs_board/app/src/led/led.hpp"
#  include "firmware/rmcs_board/app/src/timer/timer.hpp"
#  include "firmware/rmcs_board/app/src/uart/uart.hpp"
#  include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"
#  include "host_link.hpp"

int main() {
    using namespace librmcs::firmware; // NOLINT(google-build-using-namespace)

    {
        const utility::InterruptLockGuard guard;

        board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
        uplink_doorbell_init();
        dma_mgr_init();

        // Same rationale as the USB app: streaming writes bypass D-cache
        // allocation, keeping it available for hot control structures.
        l1c_dc_enable_writearound();

        led::led.init();
        timer::timer.init();

        // The link must exist before the driver ISRs can serialize into it.
        ecat::host_link.init();

        for (auto& can : can::can_array)
            can.init();

        for (auto& board_uart : uart::uart_array)
            board_uart.init();
    }

    auto& channel = ecat::xcore_channel_wait();

    uint32_t last_epoch = channel.link_epoch.load(std::memory_order::acquire);
    uint32_t last_tick = 0;
    std::byte down_buffer[256];

    while (true) {
        // Host -> fieldbus: drain the down ring into the deserializer.
        const std::size_t received = channel.down.pop(down_buffer);
        if (received != 0)
            ecat::host_link->handle_downlink({down_buffer, received});

        // SAFEOP -> OP re-entry on core0 restarts the PD stream; drop the
        // session so the host re-handshakes (session policy, see host_link).
        const uint32_t epoch = channel.link_epoch.load(std::memory_order::acquire);
        if (epoch != last_epoch) {
            last_epoch = epoch;
            ecat::host_link->handle_link_restart();
        }

        // Fieldbus -> host: pump serialized batches into the up ring, and poke
        // core0 the instant one lands so it maps the reply into the ESC without
        // waiting for the next SSC MainLoop pass.
        if (ecat::host_link->try_transmit(channel.up))
            uplink_doorbell_ring();

        for (auto& board_uart : uart::uart_array)
            board_uart->try_transmit();

        // LED bookkeeping at the 1 kHz tick pace, out of ISR context (same
        // reasoning as the USB app main loop).
        const uint32_t tick = timer::timer->tick_count();
        if (tick != last_tick) {
            last_tick = tick;
            led::led->set_host_connected(ecat::host_link->session_established());
            led::led->update(tick);
        }
    }
}

# endif

#endif
