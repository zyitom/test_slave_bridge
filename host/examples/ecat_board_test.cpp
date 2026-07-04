// P2 validation tool for the rmcs_board EtherCAT stream bridge: exercises the
// FULL librmcs protocol stack over EtherCAT (session handshake + keepalive,
// CAN0 = the bridge's MCAN4, UART0 = UART1 on PY06/PY07) through the public
// RmcsBoardEcatBridge board class -- the same API shape as every USB board.
//
// Pair it with the protocol firmware image (the default ecat superbuild, NOT
// the loopback variant used by ecat_stream_latency). Simplest bench setup:
// jumper the UART header PY06<->PY07 so every transmitted UART frame echoes
// back; CAN frames additionally appear if a bus partner acknowledges them.
//
// Build:  cmake -DLIBRMCS_ENABLE_SOEM=ON ... (see host/CMakeLists.txt)
// Run:    sudo ./ecat_board_test <interface> [seconds]

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>
#include <thread>

#include <librmcs/board/rmcs_board_ecat_bridge.hpp>

namespace {

constexpr auto kSendPeriod = std::chrono::milliseconds{100};

void print_bytes(std::span<const std::byte> bytes) {
    for (const std::byte b : bytes)
        printf("%02X ", static_cast<unsigned>(std::to_integer<uint8_t>(b)));
}

class Receiver final : public librmcs::board::RmcsBoardEcatBridge::Callback {
public:
    std::atomic<uint64_t> can_frames{0};
    std::atomic<uint64_t> uart_bytes{0};

private:
    // These callbacks run on the EtherCAT busy-poll cycle thread: a printf per
    // frame would throttle the cycle (and with it the whole link) to console
    // speed under CAN load. Print the first few for eyeballing, then count.
    static constexpr uint64_t kPrintLimit = 20;

    void can0_receive_callback(const librmcs::data::CanDataView& data) override {
        const uint64_t n = can_frames.fetch_add(1, std::memory_order_relaxed);
        if (n >= kPrintLimit) {
            if (n == kPrintLimit)
                printf("[CAN0 RX] ... (further frames counted silently)\n");
            return;
        }
        printf(
            "[CAN0 RX] id=0x%X%s dlc=%zu data=", data.can_id, data.is_extended_can_id ? " ext" : "",
            data.can_data.size());
        print_bytes(data.can_data);
        printf("\n");
    }

    void uart0_receive_callback(const librmcs::data::UartDataView& data) override {
        const uint64_t total =
            uart_bytes.fetch_add(data.uart_data.size(), std::memory_order_relaxed);
        if (total >= kPrintLimit * 16) {
            static std::atomic<bool> announced{false};
            if (!announced.exchange(true))
                printf("[UART0 RX] ... (further bytes counted silently)\n");
            return;
        }
        printf("[UART0 RX] %zu bytes: ", data.uart_data.size());
        print_bytes(data.uart_data);
        printf("| \"");
        for (const std::byte b : data.uart_data) {
            const auto c = std::to_integer<unsigned char>(b);
            putchar((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
        }
        printf("\"\n");
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <interface> [seconds]\n", argv[0]);
        return 1;
    }
    const int duration_s = argc > 2 ? std::atoi(argv[2]) : 10;

    Receiver receiver;
    try {
        // Session establishment happens inside the constructor; returning at
        // all means EtherCAT is OPERATIONAL and the protocol handshake passed.
        librmcs::board::RmcsBoardEcatBridge board{argv[1], receiver};
        printf("EtherCAT bridge connected on %s, session established.\n", argv[1]);
        printf(
            "Sending CAN0 + UART0 every %lld ms for %d s...\n",
            static_cast<long long>(kSendPeriod.count()), duration_s);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{duration_s};
        for (uint32_t counter = 0; std::chrono::steady_clock::now() < deadline; ++counter) {
            const std::byte can_payload[4] = {
                static_cast<std::byte>(counter >> 24), static_cast<std::byte>(counter >> 16),
                static_cast<std::byte>(counter >> 8), static_cast<std::byte>(counter)};
            const std::string text = "ecat " + std::to_string(counter) + "\n";

            board.start_transmit()
                .can0_transmit({.can_id = 0x123, .can_data = can_payload})
                .uart0_transmit(
                    {.uart_data = std::as_bytes(std::span{text.data(), text.size()}),
                     .idle_delimited = true});

            std::this_thread::sleep_for(kSendPeriod);
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    printf(
        "\ndone: %llu CAN frames, %llu UART bytes received.\n",
        static_cast<unsigned long long>(receiver.can_frames.load()),
        static_cast<unsigned long long>(receiver.uart_bytes.load()));
    return 0;
}
