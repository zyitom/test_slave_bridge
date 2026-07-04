#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <board.h>

#include "xcore_channel.hpp"

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

#if defined(RMCS_ECAT_CORE1_LOOPBACK) && RMCS_ECAT_CORE1_LOOPBACK

int main() {
    board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO

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
    }

    return 0;
}

#else

# include <hpm_dma_mgr.h>
# include <hpm_l1c_drv.h>

# include "firmware/rmcs_board/app/src/can/can.hpp"
# include "firmware/rmcs_board/app/src/led/led.hpp"
# include "firmware/rmcs_board/app/src/timer/timer.hpp"
# include "firmware/rmcs_board/app/src/uart/uart.hpp"
# include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"
# include "host_link.hpp"

int main() {
    using namespace librmcs::firmware; // NOLINT(google-build-using-namespace)

    {
        const utility::InterruptLockGuard guard;

        board_init_core1(); // includes board_init_pmp(): SHARE_RAM non-cacheable + AMO
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

        // Fieldbus -> host: pump serialized batches into the up ring.
        ecat::host_link->try_transmit(channel.up);

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

#endif
