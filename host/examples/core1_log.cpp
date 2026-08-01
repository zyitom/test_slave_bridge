// Print core1's diagnostic log, relayed over USB.
//
// core1 must never printf (firmware/rmcs_board/ecat/common/xcore_diag.hpp
// explains why a single printf there corrupts core0's vector table), so its only
// log path is a SHARE_RAM ring that core0 drains. The default sink for that ring
// is the board console -- a UART on the FT2232 debug header, which is not
// populated on the boards this is brought up on. A firmware built with
// LIBRMCS_DIAG_OVER_USB=ON relays the ring as UART0 uplink frames instead, and
// this prints them.
//
// Use it to watch what the EtherCAT core says at boot: the channel version
// handshake, the emulated-EEPROM/SII decision (including a first-boot rewrite
// through the cross-core flash RPC), SSC bring-up, PD sizes and the periodic
// ecat_time_ms heartbeat that shows the GPTMR timebase is running.
//
// Build: cmake --preset linux-release -S host -DBUILD_EXAMPLES=ON
// Run:   sudo ./core1_log [seconds]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

#include <librmcs/board/rmcs_board_hpm6e8y.hpp>

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

class Logger final : public librmcs::board::RmcsBoardHpm6e8y::Callback {
public:
    std::atomic<uint64_t> bytes{0};

private:
    void uart0_receive_callback(const librmcs::data::UartDataView& data) override {
        bytes.fetch_add(data.uart_data.size(), std::memory_order_relaxed);
        // Written straight through, unbuffered by line: core1's messages are
        // already newline-terminated and a partially delivered line is exactly
        // what you want to see when it stops mid-sentence.
        for (const std::byte byte : data.uart_data)
            putchar(std::to_integer<unsigned char>(byte));
        fflush(stdout);
    }
};

} // namespace

int main(int argc, char** argv) {
    const int duration_s = argc > 1 ? std::atoi(argv[1]) : 30;
    if (duration_s <= 0) {
        fprintf(stderr, "seconds must be positive\n");
        return 1;
    }

    std::signal(SIGINT, on_sigint);

    Logger logger;
    try {
        librmcs::board::RmcsBoardHpm6e8y board{logger};
        printf("--- core1 log (session up; %d s, Ctrl-C to stop) ---\n", duration_s);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{duration_s};
        while (std::chrono::steady_clock::now() < deadline
               && !g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }

        printf(
            "\n--- end of log, %llu byte(s) ---\n",
            static_cast<unsigned long long>(logger.bytes.load()));
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
