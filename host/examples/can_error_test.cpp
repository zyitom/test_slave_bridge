// CAN error light-code test for the HPM5321 DualCan board.
//
// Continuously transmits a frame on CAN0 (MCAN0) and CAN1 (MCAN3) so the board
// is always driving the bus -- CAN errors only arise while a controller is
// actually transmitting/receiving, so an idle bus reports nothing.  Use this to
// verify the indicator-LED light language:
//
//   off          = healthy / no errors
//   1 pulse      = ACK error    (no node ACKing -- nothing connected on the bus)
//   2 pulses     = stuff error  (baudrate mismatch / heavy noise)
//   3 pulses     = form error   (frame format violation)
//   4 pulses     = bit error    (TX vs monitored mismatch -- CAN_H/CAN_L shorted)
//   5 pulses     = CRC error    (signal integrity)
//   fast blink   = bus-off      (controller offline after too many errors)
//
// Quick checks:
//   * Leave a bus disconnected            -> that LED should show 1 pulse (ACK).
//   * Short CAN_H to CAN_L on a bus        -> that LED should show a bit error.
// Stop with Ctrl-C.

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <librmcs/agent/rmcs_board_hpm5321_dual_can.hpp>

namespace {

constexpr unsigned kBusCount = 2;        // CAN0 + CAN1
constexpr bool kUseCanFd = false;        // classic CAN 2.0; set true only with an FD partner
// Must be unique on the bus: two transmitters sending the SAME id with different
// data collide after arbitration -> bit errors -> SIGNAL (double blink).  Pick an
// id no other node (e.g. the USB2CAN) uses.
constexpr uint32_t kTestCanId = 0x321;
constexpr auto kSendPeriod = std::chrono::milliseconds{5};  // ~200 Hz per bus

std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }

}  // namespace

class CanErrorTest : public librmcs::agent::RmcsBoardHpm5321DualCan {
public:
    CanErrorTest()
        : librmcs::agent::RmcsBoardHpm5321DualCan{
              {}, {.dangerously_skip_version_checks = true}} {}

    void send_test_frame(unsigned bus) {
        std::array<std::byte, 8> frame{
            std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
            std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
        const librmcs::data::CanDataView data{
            .can_id = kTestCanId, .can_data = frame, .is_fdcan = false};
        auto builder = start_transmit();
        if (bus == 0)
            builder.can0_transmit(data);
        else
            builder.can1_transmit(data);
    }

    uint64_t rx_count(unsigned bus) const { return rx_count_[bus].load(); }

private:
    void can0_receive_callback(const librmcs::data::CanDataView&) override { ++rx_count_[0]; }
    void can1_receive_callback(const librmcs::data::CanDataView&) override { ++rx_count_[1]; }

    std::atomic<uint64_t> rx_count_[kBusCount]{};
};

int main() {
    std::signal(SIGINT, on_sigint);

    printf("CAN error light-code test — HPM5321 DualCan (CAN-FD=%s)\n",
           kUseCanFd ? "on" : "off");
    printf("  Transmitting id 0x%03X on CAN0 and CAN1 at ~200 Hz.\n", kTestCanId);
    printf("  Watch the per-bus indicator LEDs:\n");
    printf("    disconnected bus -> 1 pulse (ACK error)\n");
    printf("    CAN_H/CAN_L short -> bit error\n");
    printf("  Ctrl-C to stop.\n");
    printf("Connecting ...\n");

    CanErrorTest agent;
    printf("Connected.  Sending ...\n");

    std::array<uint64_t, kBusCount> tx_count{};
    auto next_send = std::chrono::steady_clock::now();
    auto last_report = next_send;

    while (g_running.load()) {
        const auto now = std::chrono::steady_clock::now();

        for (unsigned i = 0; i < kBusCount; ++i) {
            agent.send_test_frame(i);
            ++tx_count[i];
        }

        next_send += kSendPeriod;
        if (next_send < now)
            next_send = now;
        std::this_thread::sleep_until(next_send);

        if (now - last_report >= std::chrono::seconds{1}) {
            last_report = now;
            printf("CAN0: tx %lu rx %lu | CAN1: tx %lu rx %lu\n", tx_count[0],
                   agent.rx_count(0), tx_count[1], agent.rx_count(1));
            tx_count = {};
        }
    }

    printf("\nStopped.\n");
    return 0;
}
