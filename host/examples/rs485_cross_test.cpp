#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>

#include <librmcs/board/mc02.hpp>

// Cross-board RS-485 test for two mc02 boards wired A-A / B-B on one of the two
// RS-485 ports. RMCS_RS485_PORT picks which: 2 (the default) is USART3 --
// connector P5, transceiver U6, DE on PB14 -- and 1 is USART2, transceiver U5,
// DE on PD4, which owns kUart0. Both power up at 4800000 and are electrically
// the same circuit, so every test below applies unchanged to either.
// Needs firmware built with -DLIBRMCS_APP_RS485_ENABLE=ON at both ends: without it the firmware's
// downlink switch has no kUart4 case, returns false, and the deserializer tears
// the session down rather than ignoring the frame.
//
// Why a separate program from uart_cross_test: that one addresses ports through
// the board-agnostic BoardTransmitter, whose port indices come from
// Spec::kUarts, and kUart0/kUart4 are deliberately absent from that table so
// generic code walking every descriptor cannot take a default firmware offline.
// Reaching this port means talking to librmcs::board::Mc02 directly.
//
// The two properties a same-board loopback cannot check, and the reason every
// exchange below crosses a wire between two independent boards:
//
//   - Direction control. DE is raised by the USART itself (CR3.DEM, set by
//     HAL_RS485Ex_Init), so a board that never releases the bus still passes any
//     test where only one board transmits. Both directions are exercised, and
//     the sender is checked for silence while it transmits.
//   - Baudrate. A silently ignored config request leaves both ends at whatever
//     CubeMX programmed -- 4800000 for USART3 -- and a loopback still passes at
//     every requested rate (see firmware/mc02/AGENTS.md).
//
// Framing contract the firmware presents: RxBuffer::try_dequeue() publishes a
// chunk as soon as kMinFragmentSize (32) bytes are in the ring, without waiting
// for the line to go idle. A message is "everything up to and including the next
// chunk flagged idle_delimited", never "one chunk" -- and when the message
// length is an exact multiple of 32 that final chunk is empty.
//
// Half-duplex pacing is the firmware's, not this program's: TxBuffer with
// half_duplex=true releases one queued packet per bus transaction, gated until
// either the peer's IDLE counter moves or kTurnaroundDeadline (1000 us) expires.
// test_turnaround below measures exactly that gate.

namespace {

using librmcs::board::Mc02;
using librmcs::data::UartDataView;
using Clock = std::chrono::steady_clock;

constexpr uint32_t k_base_baudrate = 115200;
constexpr auto k_message_timeout = std::chrono::milliseconds(500);

// Staging buffer for a UartRs485 port is kRs485BufferSize (256) bytes; stay
// under it so a size sweep tests fragmentation rather than backpressure.
constexpr std::size_t k_max_payload = 200;

uint32_t g_baudrate = k_base_baudrate;
// Which RS-485 port the whole test drives. 2 (USART3, connector P5) is the
// default because that is the port the original rig was wired for and every
// number in this file's history came from. 1 selects USART2 / transceiver U5,
// which owns kUart0 and is otherwise identical electrically -- same RE#-tied-to-DE
// topology, same on-board 120R, same 4800000 power-up baudrate.
int g_port = 2;
int g_ping_pong_rounds = 200;

std::vector<std::pair<std::string, std::string>> enumerate_boards() {
    std::vector<std::pair<std::string, std::string>> found; // serial, product
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir)
        return found;
    while (const dirent* entry = readdir(dir)) {
        const std::string base = std::string{"/sys/bus/usb/devices/"} + entry->d_name;
        const auto read_line = [&](const char* leaf) -> std::string {
            const std::string path = base + "/" + leaf;
            FILE* file = fopen(path.c_str(), "re");
            if (!file)
                return {};
            char buffer[256] = {};
            if (!fgets(buffer, sizeof(buffer), file)) {
                fclose(file);
                return {};
            }
            fclose(file);
            std::string value{buffer};
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
                value.pop_back();
            return value;
        };
        if (read_line("idVendor") != "a11c" || read_line("idProduct") != "d402")
            continue;
        const std::string serial = read_line("serial");
        if (!serial.empty())
            found.emplace_back(serial, read_line("product"));
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    return found;
}

// Distinguishable at every position: a plain incrementing ramp repeats every 256
// bytes, so a dropped or duplicated 256-byte run would compare equal.
std::vector<std::byte> make_pattern(std::size_t size, uint32_t seed) {
    std::vector<std::byte> out(size);
    uint32_t state = 0x9E3779B9U ^ seed;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525U + 1013904223U;
        out[i] = static_cast<std::byte>((state >> 24) ^ static_cast<uint32_t>(i));
    }
    return out;
}

std::size_t
    first_difference(std::span<const std::byte> expected, std::span<const std::byte> actual) {
    const auto common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i])
            return i;
    }
    return common;
}

std::string hex_preview(std::span<const std::byte> data, std::size_t limit = 16) {
    std::string out;
    char scratch[8];
    for (std::size_t i = 0; i < std::min(limit, data.size()); ++i) {
        std::snprintf(scratch, sizeof(scratch), "%02x ", static_cast<unsigned>(data[i]));
        out += scratch;
    }
    if (data.size() > limit)
        out += "...";
    return out;
}

// One board, plus reassembly state for the USART3 RS-485 port. Traffic on any
// other channel is counted rather than dropped, so a firmware that misroutes a
// downlink shows up as a number instead of a silent pass.
class Node final : public Mc02::Callback {
public:
    Node(std::string tag, std::string_view serial)
        : tag_(std::move(tag)) {
        board_ = std::make_unique<Mc02>(*this, serial);
    }

    ~Node() override { stop_responder(); }

    const std::string& tag() const { return tag_; }

    void reset() {
        const std::lock_guard guard{mutex_};
        partial_.clear();
        messages_.clear();
        all_bytes_.clear();
        chunk_count_ = 0;
        other_channel_chunks_ = 0;
        other_channel_name_ = nullptr;
        dbus_chunks_ = 0;
        dbus_bytes_ = 0;
    }

    void send(std::span<const std::byte> payload, bool idle_delimited = true) {
        const std::lock_guard guard{transmit_mutex_};
        if (g_port == 1)
            board_->start_transmit().rs485_1_transmit(
                {.uart_data = payload, .idle_delimited = idle_delimited});
        else
            board_->start_transmit().rs485_2_transmit(
                {.uart_data = payload, .idle_delimited = idle_delimited});
    }

    void set_baudrate(uint32_t baudrate) {
        const std::lock_guard guard{transmit_mutex_};
        if (g_port == 1)
            board_->start_transmit().rs485_1_config({.baudrate = baudrate});
        else
            board_->start_transmit().rs485_2_config({.baudrate = baudrate});
    }

    std::size_t message_count() {
        const std::lock_guard guard{mutex_};
        return messages_.size();
    }

    std::size_t chunk_count() {
        const std::lock_guard guard{mutex_};
        return chunk_count_;
    }

    std::size_t byte_count() {
        const std::lock_guard guard{mutex_};
        return all_bytes_.size();
    }

    std::size_t pending_bytes() {
        const std::lock_guard guard{mutex_};
        return partial_.size();
    }

    std::size_t other_channel_chunks() {
        const std::lock_guard guard{mutex_};
        return other_channel_chunks_;
    }

    const char* other_channel_name() {
        const std::lock_guard guard{mutex_};
        return other_channel_name_ ? other_channel_name_ : "none";
    }

    std::pair<std::size_t, std::size_t> dbus_traffic() {
        const std::lock_guard guard{mutex_};
        return {dbus_chunks_, dbus_bytes_};
    }

    std::vector<std::vector<std::byte>> take_messages() {
        const std::lock_guard guard{mutex_};
        return std::exchange(messages_, {});
    }

    // Waits for at least `count` complete messages. Returns what arrived, which
    // may be fewer on timeout -- the caller reports the shortfall.
    std::vector<std::vector<std::byte>>
        wait_for_messages(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        condition_.wait_for(lock, timeout, [&] { return messages_.size() >= count; });
        return std::exchange(messages_, {});
    }

    // Echoes every complete message back onto the bus from a dedicated thread.
    // Not from the receive callback: that runs on the transport's completion
    // thread, and submitting a transfer from inside a completion is a
    // reentrancy question this test has no reason to answer.
    void start_responder() {
        stop_responder();
        responder_running_ = true;
        responder_ = std::thread{[this] {
            while (responder_running_) {
                std::vector<std::byte> reply;
                {
                    std::unique_lock lock{mutex_};
                    condition_.wait_for(lock, std::chrono::milliseconds(20), [&] {
                        return !reply_queue_.empty() || !responder_running_;
                    });
                    if (!responder_running_)
                        return;
                    if (reply_queue_.empty())
                        continue;
                    reply = std::move(reply_queue_.front());
                    reply_queue_.pop_front();
                }
                send(reply);
            }
        }};
    }

    void stop_responder() {
        if (!responder_.joinable())
            return;
        {
            const std::lock_guard guard{mutex_};
            responder_running_ = false;
        }
        condition_.notify_all();
        responder_.join();
        const std::lock_guard guard{mutex_};
        reply_queue_.clear();
    }

private:
    void rs485_2_receive_callback(const UartDataView& data) override {
        if (g_port != 2) {
            count_other(data, "RS485-2");
            return;
        }
        accept(data);
    }

    // The port under test. Everything below reassembles idle-delimited messages
    // out of the chunk stream; which physical port feeds it is g_port's business.
    void accept(const UartDataView& data) {
        const std::lock_guard guard{mutex_};
        ++chunk_count_;
        all_bytes_.insert(all_bytes_.end(), data.uart_data.begin(), data.uart_data.end());
        partial_.insert(partial_.end(), data.uart_data.begin(), data.uart_data.end());
        if (!data.idle_delimited)
            return;
        auto message = std::exchange(partial_, {});
        if (responder_running_)
            reply_queue_.push_back(message);
        messages_.push_back(std::move(message));
        condition_.notify_all();
    }

    // USART2's RS-485 port and the three ordinary UARTs. Nothing this test sends
    // should land here; a nonzero count means the firmware routed a downlink or
    // an uplink to the wrong channel id.
    void rs485_1_receive_callback(const UartDataView& data) override {
        if (g_port == 1) {
            accept(data);
            return;
        }
        count_other(data, "RS485-1");
    }
    void uart1_receive_callback(const UartDataView& data) override { count_other(data, "UART1"); }
    void uart2_receive_callback(const UartDataView& data) override { count_other(data, "UART2"); }
    void uart3_receive_callback(const UartDataView& data) override { count_other(data, "UART3"); }
    // DBUS is UART5, RX-only at 100000 8E1, and it shares the uplink ring with
    // every other channel. An unconnected receiver floats, so it can fill that
    // ring on its own -- which looks exactly like an RS-485 fault from the
    // outside, because the only symptom either produces is the LED. Counted
    // separately for that reason: "the board is busy" and "the 485 port is busy"
    // are different findings.
    void dbus_receive_callback(const UartDataView& data) override {
        if (data.uart_data.empty())
            return;
        const std::lock_guard guard{mutex_};
        dbus_chunks_ += 1;
        dbus_bytes_ += data.uart_data.size();
    }

    void count_other(const UartDataView& data, const char* name) {
        if (data.uart_data.empty())
            return;
        const std::lock_guard guard{mutex_};
        ++other_channel_chunks_;
        other_channel_name_ = name;
    }

    std::string tag_;
    std::mutex mutex_;
    std::mutex transmit_mutex_;
    std::condition_variable condition_;

    std::vector<std::byte> partial_;
    std::vector<std::byte> all_bytes_;
    std::vector<std::vector<std::byte>> messages_;
    std::deque<std::vector<std::byte>> reply_queue_;
    std::size_t chunk_count_ = 0;
    std::size_t other_channel_chunks_ = 0;
    const char* other_channel_name_ = nullptr;
    std::size_t dbus_chunks_ = 0;
    std::size_t dbus_bytes_ = 0;

    bool responder_running_ = false;
    std::thread responder_;

    std::unique_ptr<Mc02> board_;
};

int g_failures = 0;

void report(bool ok, const std::string& what, const std::string& detail = {}) {
    printf(
        "  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " -- ",
        detail.c_str());
    fflush(stdout);
    if (!ok)
        ++g_failures;
}

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(60)); }

// One payload across the bus in one direction, checked for content, for framing
// (exactly one idle-delimited message), and for the sender staying silent --
// RE# is tied to DE on this board, so our own transmission must not come back.
bool exchange_once(Node& from, Node& to, std::size_t size, uint32_t seed, std::string& detail) {
    from.reset();
    to.reset();
    const auto payload = make_pattern(size, seed);
    from.send(payload);

    const auto received = to.wait_for_messages(1, k_message_timeout);
    // A late second message would arrive after the wait returned, so give the
    // line a moment before judging the count.
    settle();
    const auto extra = to.take_messages();

    char scratch[256];
    if (received.empty()) {
        std::snprintf(
            scratch, sizeof(scratch), "%zu bytes sent, nothing arrived (%zu partial)", size,
            to.pending_bytes());
        detail = scratch;
        return false;
    }
    if (received.size() + extra.size() != 1) {
        std::snprintf(
            scratch, sizeof(scratch), "expected 1 message, got %zu",
            received.size() + extra.size());
        detail = scratch;
        return false;
    }
    const auto& message = received.front();
    if (message.size() != size || first_difference(payload, message) != size) {
        const auto index = first_difference(payload, message);
        std::snprintf(
            scratch, sizeof(scratch), "sent %zu bytes, got %zu, first difference at byte %zu", size,
            message.size(), index);
        detail = scratch;
        detail += "\n         sent: "
                + hex_preview(std::span{payload}.subspan(std::min(index, payload.size())));
        detail += "\n         recv: "
                + hex_preview(std::span{message}.subspan(std::min(index, message.size())));
        return false;
    }
    if (from.byte_count() != 0) {
        std::snprintf(
            scratch, sizeof(scratch),
            "sender saw %zu bytes of its own transmission (RE# not following DE?)",
            from.byte_count());
        detail = scratch;
        return false;
    }
    detail.clear();
    return true;
}

// Sizes chosen around kMinFragmentSize (32): a message shorter than one fragment
// arrives only because the line went idle, a message that is an exact multiple
// arrives as N full chunks plus an empty idle-delimited one, and the odd sizes
// land mid-fragment.
void test_sizes(Node& a, Node& b) {
    printf("\n== Payload sizes at %u baud ==\n", g_baudrate);
    constexpr std::size_t sizes[] = {1, 8, 31, 32, 33, 64, 96, 128, k_max_payload};
    uint32_t seed = 1;
    for (const auto size : sizes) {
        std::string detail;
        const bool forward = exchange_once(a, b, size, seed++, detail);
        report(
            forward, a.tag() + " -> " + b.tag() + ", " + std::to_string(size) + " bytes", detail);
        settle();
        const bool reverse = exchange_once(b, a, size, seed++, detail);
        report(
            reverse, b.tag() + " -> " + a.tag() + ", " + std::to_string(size) + " bytes", detail);
        settle();
    }
}

// The check a loopback cannot make. Both ends are moved together; if one end
// ignored the request the other's bytes arrive at the wrong bit rate and the
// comparison fails on content, not on silence.
void test_baudrates(Node& a, Node& b) {
    printf("\n== Baudrate sweep ==\n");
    constexpr uint32_t rates[] = {115200, 460800, 921600, 1500000, 3000000, 4800000};
    uint32_t seed = 1000;
    for (const auto rate : rates) {
        a.set_baudrate(rate);
        b.set_baudrate(rate);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::string detail;
        const bool forward = exchange_once(a, b, 64, seed++, detail);
        report(forward, std::to_string(rate) + " baud, " + a.tag() + " -> " + b.tag(), detail);
        settle();
        const bool reverse = exchange_once(b, a, 64, seed++, detail);
        report(reverse, std::to_string(rate) + " baud, " + b.tag() + " -> " + a.tag(), detail);
        settle();
    }
    a.set_baudrate(g_baudrate);
    b.set_baudrate(g_baudrate);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

// Back-to-back packets with no peer answering. The half-duplex TxBuffer holds
// each packet until the peer's IDLE counter moves or kTurnaroundDeadline
// (1000 us) expires; with a silent peer only the deadline releases them, so N
// packets should take about (N-1) ms and still arrive as N separate messages.
void test_turnaround(Node& a, Node& b) {
    printf("\n== Half-duplex turnaround (silent peer) ==\n");
    constexpr int kPackets = 8;
    a.reset();
    b.reset();
    const auto start = Clock::now();
    for (int i = 0; i < kPackets; ++i)
        a.send(make_pattern(16, static_cast<uint32_t>(2000 + i)));

    const auto messages = b.wait_for_messages(kPackets, std::chrono::milliseconds(3000));
    const auto elapsed = Clock::now() - start;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();

    char scratch[256];
    std::snprintf(
        scratch, sizeof(scratch), "%zu of %d messages in %.2f ms (%.2f ms per packet)",
        messages.size(), kPackets, elapsed_ms, elapsed_ms / kPackets);
    report(
        messages.size() == kPackets, "packets kept separate across the turnaround gate", scratch);

    bool contents_ok = messages.size() == kPackets;
    for (std::size_t i = 0; i < messages.size() && contents_ok; ++i) {
        const auto expected = make_pattern(16, static_cast<uint32_t>(2000 + i));
        contents_ok = messages[i].size() == expected.size()
                   && first_difference(expected, messages[i]) == expected.size();
    }
    report(contents_ok, "turnaround packets arrived intact and in order");
    settle();
}

// Request/response, which is what the turnaround gate is built for: the peer's
// answer raises IDLE on the initiator and releases the next packet early, so
// this should run far faster than the 1 ms deadline path above.
//
// The figure is a host-observed round trip: USB out, bus, USB in, responder
// thread wakeup, USB out, bus, USB in. It is not a bus turnaround measurement --
// the two USB traversals dominate -- but it is the number an application driving
// a 485 device through this port will actually see.
void test_ping_pong(Node& a, Node& b) {
    printf("\n== Ping-pong round trip (%d rounds) ==\n", g_ping_pong_rounds);
    a.reset();
    b.reset();
    b.start_responder();
    settle();

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(g_ping_pong_rounds));
    int lost = 0;
    int corrupt = 0;
    for (int round = 0; round < g_ping_pong_rounds; ++round) {
        const auto payload = make_pattern(16, static_cast<uint32_t>(3000 + round));
        a.take_messages();
        const auto start = Clock::now();
        a.send(payload);
        const auto replies = a.wait_for_messages(1, k_message_timeout);
        const auto elapsed = Clock::now() - start;
        if (replies.empty()) {
            ++lost;
            continue;
        }
        if (replies.front().size() != payload.size()
            || first_difference(payload, replies.front()) != payload.size())
            ++corrupt;
        samples.push_back(
            std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(elapsed).count());
    }
    b.stop_responder();

    char scratch[256];
    std::snprintf(
        scratch, sizeof(scratch), "%d lost, %d corrupt of %d", lost, corrupt, g_ping_pong_rounds);
    report(lost == 0 && corrupt == 0, "every request answered with its own payload", scratch);

    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](double fraction) {
            const auto index = static_cast<std::size_t>(fraction * (samples.size() - 1));
            return samples[index];
        };
        double sum = 0;
        for (const auto value : samples)
            sum += value;
        printf(
            "  round trip: min %.0f us, mean %.0f us, p50 %.0f us, p99 %.0f us, max %.0f us\n",
            samples.front(), sum / static_cast<double>(samples.size()), percentile(0.50),
            percentile(0.99), samples.back());
    }
    settle();
}

// Two drivers on the bus at once. RS-485 has no arbitration and both
// transceivers are push-pull, so the bytes on the wire during the overlap are
// undefined -- what matters is that neither port wedges: after the collision,
// both directions must still carry a clean message.
void test_collision_recovery(Node& a, Node& b) {
    printf("\n== Collision recovery ==\n");
    a.reset();
    b.reset();
    for (int i = 0; i < 4; ++i) {
        a.send(make_pattern(64, static_cast<uint32_t>(4000 + i)));
        b.send(make_pattern(64, static_cast<uint32_t>(5000 + i)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    char scratch[128];
    std::snprintf(
        scratch, sizeof(scratch), "%s saw %zu bytes, %s saw %zu bytes during the overlap",
        a.tag().c_str(), a.byte_count(), b.tag().c_str(), b.byte_count());
    printf("  [INFO] %s\n", scratch);

    std::string detail;
    const bool forward = exchange_once(a, b, 64, 4100, detail);
    report(forward, "link usable after collision, " + a.tag() + " -> " + b.tag(), detail);
    settle();
    const bool reverse = exchange_once(b, a, 64, 4200, detail);
    report(reverse, "link usable after collision, " + b.tag() + " -> " + a.tag(), detail);
    settle();
}

// Listen with nobody transmitting. An idle RS-485 pair that no driver is holding
// sits at whatever the termination and the leakage currents decide, which for a
// terminated link with no fail-safe bias is a differential voltage near zero --
// inside the receiver's undefined window. The receiver then outputs noise, the
// USART reads it as start bits, and the port produces uplink traffic forever
// with nothing on the bus. That is what a quiet link must be checked for, and
// the LED is the symptom: uplink_buffer_full() blinks yellow for 5 s per event,
// so a permanently yellow board is a port that never stops receiving.
void test_quiet_bus(Node& a, Node& b) {
    printf("\n== Quiet bus (nobody transmitting, 5 s) ==\n");
    a.reset();
    b.reset();
    std::this_thread::sleep_for(std::chrono::seconds(5));

    char scratch[256];
    for (Node* node : {&a, &b}) {
        const auto bytes = node->byte_count();
        std::snprintf(
            scratch, sizeof(scratch), "%s received %zu bytes in %zu chunks on a silent bus",
            node->tag().c_str(), bytes, node->chunk_count());
        report(bytes == 0, std::string{"idle line stays quiet on "} + node->tag(), scratch);
    }

    // Anything arriving here shares the uplink ring with the RS-485 port, so it
    // fills the same buffer and lights the same LED. Reported whether or not it
    // is zero: "the 485 port is silent and the board is still busy" is the
    // finding that separates a port fault from a board-level one.
    for (Node* node : {&a, &b}) {
        const auto [chunks, bytes] = node->dbus_traffic();
        std::snprintf(
            scratch, sizeof(scratch), "%s: %zu bytes in %zu chunks over 5 s", node->tag().c_str(),
            bytes, chunks);
        printf("  [INFO] DBUS (UART5, shares the uplink ring) %s\n", scratch);
    }
    fflush(stdout);
}

// Does a single small message still leave immediately, now that a bulk OUT
// transfer asks for 1024 bytes instead of 64?
//
// The hazard is specific: a transfer completes on a full 1024 bytes OR on a
// short packet. A downlink whose total is an exact multiple of the 64-byte
// endpoint has no short packet, so it would sit in the controller until enough
// further traffic arrived -- not slower, stuck. The host sets
// LIBUSB_TRANSFER_ADD_ZERO_PACKET (host/src/transport/usb/usb.cpp:422) so libusb
// appends a zero-length packet in exactly that case, which is short and ends the
// transfer. This test is here because that is a claim about two codebases
// agreeing, and it is worth more as a measurement than as a reading.
//
// Sweeping payload size one byte at a time crosses every 64-byte boundary in
// transfer size without needing to know the per-record header width. A stall
// shows up as lost messages at one specific size; an added delay shows up as a
// step in the round-trip curve at the same place.
void test_latency_vs_size(Node& a, Node& b) {
    printf("\n== Round-trip vs message size (ZLP / transfer-boundary check) ==\n");
    int kRounds = 20;
    if (const char* value = std::getenv("RMCS_RS485_ROUNDS"))
        kRounds = std::atoi(value);
    constexpr std::size_t kMaxSize = 200;

    a.set_baudrate(4800000);
    b.set_baudrate(4800000);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    b.start_responder();
    settle();

    std::vector<std::pair<std::size_t, double>> curve;
    std::vector<std::size_t> stalled;
    for (std::size_t size = 1; size <= kMaxSize; ++size) {
        std::vector<double> samples;
        int lost = 0;
        for (int round = 0; round < kRounds; ++round) {
            const auto payload = make_pattern(size, static_cast<uint32_t>(9000 + size));
            a.take_messages();
            const auto start = Clock::now();
            a.send(payload);
            const auto replies = a.wait_for_messages(1, k_message_timeout);
            if (replies.empty()) {
                ++lost;
                continue;
            }
            samples.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                  Clock::now() - start)
                                  .count());
        }
        if (lost != 0)
            stalled.push_back(size);
        if (!samples.empty()) {
            std::sort(samples.begin(), samples.end());
            curve.emplace_back(size, samples[samples.size() / 2]);
        }
    }
    b.stop_responder();

    char scratch[256];
    std::snprintf(
        scratch, sizeof(scratch), "%zu of %zu sizes lost messages", stalled.size(), kMaxSize);
    report(stalled.empty(), "every message size round-trips", scratch);
    if (!stalled.empty()) {
        printf("  stalled sizes:");
        for (const auto size : stalled)
            printf(" %zu", size);
        printf("\n");
    }

    // The line time grows with size, so the curve must slope. What would betray a
    // transfer that waited is a jump at one size that the neighbouring sizes do
    // not share, so report the largest single-byte step rather than the slope.
    double worst_step = 0;
    std::size_t worst_at = 0;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const double step = curve[i].second - curve[i - 1].second;
        if (step > worst_step) {
            worst_step = step;
            worst_at = curve[i].first;
        }
    }
    printf(
        "  p50 round trip: %zu B %.0f us -> %zu B %.0f us\n", curve.front().first,
        curve.front().second, curve.back().first, curve.back().second);
    // Full curve on request: the endpoints alone cannot show whether a step sits
    // at a 64-byte boundary, which is the whole question here.
    if (std::getenv("RMCS_RS485_CURVE") != nullptr) {
        for (const auto& [size, us] : curve)
            printf("CURVE %zu %.0f\n", size, us);
    }
    std::snprintf(
        scratch, sizeof(scratch), "largest one-byte jump %.0f us at size %zu", worst_step,
        worst_at);
    // One 64-byte packet is 133 us of line time at 4.8 Mbaud; a transfer that
    // waited for more traffic would cost far more than that, so anything under
    // 200 us is the line, not the USB layer.
    report(worst_step < 200.0, "no size pays a transfer-boundary penalty", scratch);

    a.set_baudrate(g_baudrate);
    b.set_baudrate(g_baudrate);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    settle();
}

void test_channel_isolation(Node& a, Node& b) {
    printf("\n== Channel isolation ==\n");
    char scratch[128];
    std::snprintf(
        scratch, sizeof(scratch), "%zu chunks on %s, %zu on %s", a.other_channel_chunks(),
        a.tag().c_str(), b.other_channel_chunks(), b.tag().c_str());
    report(
        a.other_channel_chunks() == 0 && b.other_channel_chunks() == 0,
        "no traffic leaked onto the other UART channels", scratch);
}

} // namespace

int main(int argc, char** argv) {
    if (const char* value = std::getenv("RMCS_RS485_PORT"))
        g_port = std::atoi(value);
    if (g_port != 1 && g_port != 2) {
        printf("RMCS_RS485_PORT must be 1 (USART2, connector U5) or 2 (USART3, P5)\n");
        return 1;
    }
    if (const char* value = std::getenv("RMCS_RS485_BAUDRATE"))
        g_baudrate = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    if (const char* value = std::getenv("RMCS_RS485_ROUNDS"))
        g_ping_pong_rounds = std::atoi(value);
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool sweep_only = mode == "baudrate";
    // Observes without ever transmitting, so it can answer "is the port
    // producing traffic that nobody sent" on its own.
    const bool quiet_only = mode == "quiet";
    const bool latency_only = mode == "latency";

    const auto boards = enumerate_boards();
    printf("mc02 boards found: %zu\n", boards.size());
    for (const auto& [serial, product] : boards)
        printf("  %s  %s\n", serial.c_str(), product.c_str());
    if (boards.size() < 2) {
        printf(
            "\nNeed two mc02 boards wired A-A / B-B on the RS-485 port under test "
            "(RMCS_RS485_PORT=1 -> USART2/U5, 2 -> USART3/P5).\n");
        return 1;
    }

    printf("\nOpening boards...\n");
    Node a{"A", boards[0].first};
    Node b{"B", boards[1].first};
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    printf(
        "Driving RS-485 port %d (%s). Setting both ends to %u baud "
        "(both USARTs power up at 4800000).\n",
        g_port, g_port == 1 ? "USART2, transceiver U5" : "USART3, connector P5", g_baudrate);
    a.set_baudrate(g_baudrate);
    b.set_baudrate(g_baudrate);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    if (latency_only) {
        test_latency_vs_size(a, b);
        printf(
            "\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
            g_failures == 1 ? "" : "s");
        return g_failures == 0 ? 0 : 1;
    }

    if (quiet_only) {
        test_quiet_bus(a, b);
        printf(
            "\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
            g_failures == 1 ? "" : "s");
        return g_failures == 0 ? 0 : 1;
    }

    // Cheapest possible proof the wiring and the firmware build are right, run
    // before anything that would be confusing to read on a dead link.
    std::string detail;
    a.reset();
    b.reset();
    const bool link = exchange_once(a, b, 8, 0, detail);
    printf("\n== Link check ==\n");
    report(link, "A -> B carries a short message", detail);
    if (!link) {
        printf("\nNothing crossed the bus. Check, in order:\n"
               "  1. Both boards flashed with -DLIBRMCS_APP_RS485_ENABLE=ON.\n"
               "     A firmware without it drops the session on the first kUart4 frame,\n"
               "     which shows up as an immediate USB error rather than as silence.\n"
               "  2. A-A and B-B, not A-B (a crossed pair is silent, not corrupt).\n"
               "  3. Termination: 120R is fitted on-board at each end (R16), so a short\n"
               "     link has 60R total, which is correct for two ends.\n");
        return 1;
    }
    settle();

    if (sweep_only) {
        test_baudrates(a, b);
    } else {
        test_sizes(a, b);
        test_baudrates(a, b);
        test_turnaround(a, b);
        test_ping_pong(a, b);
        test_collision_recovery(a, b);
        test_latency_vs_size(a, b);
        test_quiet_bus(a, b);
        test_channel_isolation(a, b);
    }

    printf(
        "\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
        g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
