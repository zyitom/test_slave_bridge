#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <thread>

#include <librmcs/agent/rmcs_board_hpm5321.hpp>

// Non-interactive tester for the HPM5321 board. After connecting it just keeps
// sending one CAN0 frame and one UART0 message every period, and prints whatever
// it receives back. Exercises the only two peripherals the board exposes.
// Stop with Ctrl-C.

namespace {

constexpr auto k_send_period = std::chrono::milliseconds{500};

std::atomic<bool> g_running{true};

void on_sigint(int) { g_running.store(false); }

void print_bytes(std::span<const std::byte> bytes) {
    for (const std::byte b : bytes)
        printf("%02X ", static_cast<unsigned>(std::to_integer<uint8_t>(b)));
}

} // namespace

class Hpm5321Tester : public librmcs::agent::RmcsBoardHpm5321 {
public:
    Hpm5321Tester()
        : librmcs::agent::RmcsBoardHpm5321{{}, {.dangerously_skip_version_checks = true}} {}

    void send_round(uint32_t counter) {
        // CAN0: id 0x123, 4 payload bytes carrying the counter (big-endian).
        const std::byte can_payload[4] = {
            static_cast<std::byte>(counter >> 24), static_cast<std::byte>(counter >> 16),
            static_cast<std::byte>(counter >> 8), static_cast<std::byte>(counter)};
        start_transmit().can0_transmit({.can_id = 0x123, .can_data = can_payload});

        // UART0: a short ASCII line carrying the counter.
        const std::string text = "hello " + std::to_string(counter) + "\n";
        start_transmit().uart0_transmit(
            {.uart_data = std::as_bytes(std::span{text.data(), text.size()}),
             .idle_delimited = true});

        printf("[TX #%u] CAN0 id=0x123 + UART0 \"hello %u\"\n", counter, counter);
    }

private:
    void can0_receive_callback(const librmcs::data::CanDataView& data) override {
        printf(
            "[CAN0 RX] id=0x%X%s dlc=%zu data=", data.can_id,
            data.is_extended_can_id ? " ext" : "", data.can_data.size());
        print_bytes(data.can_data);
        printf("\n");
    }

    void uart0_receive_callback(const librmcs::data::UartDataView& data) override {
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

int main() {
    std::signal(SIGINT, on_sigint);

    printf("HPM5321 tester - connecting...\n");
    Hpm5321Tester tester;
    printf("Connected. Sending CAN0 + UART0 every %lldms. Ctrl-C to stop.\n",
        static_cast<long long>(k_send_period.count()));

    for (uint32_t counter = 0; g_running.load(); ++counter) {
        tester.send_round(counter);
        std::this_thread::sleep_for(k_send_period);
    }

    printf("\nStopped.\n");
    return 0;
}
