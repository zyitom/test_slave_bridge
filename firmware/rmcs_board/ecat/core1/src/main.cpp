#include <cstddef>
#include <span>

#include <board.h>

#include "xcore_channel.hpp"

// Core1 skeleton: lossless echo of the host stream.
//
// Phase-P1 validation only. It proves ESC + PD stop-and-wait ARQ +
// shared-memory rings + dual-core boot end to end with zero peripheral code:
// whatever byte stream the master sends comes back byte-exact. Later phases
// replace the echo loop with the fieldbus application (MCAN/UART drivers plus
// the librmcs protocol serializer/deserializer), which consumes `down` and
// produces `up` through exactly the same two rings.
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
        while (!channel.up.try_push(chunk)) {
        }
    }

    return 0;
}
