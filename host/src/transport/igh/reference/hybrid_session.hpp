#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <librmcs/ecat/hybrid_pd.hpp>
#include <librmcs/ecat/pd_stream.hpp>
#include <librmcs/data/datas.hpp>

namespace librmcs::host::transport::igh::reference {

namespace ecat = librmcs::ecat;

// Minimal librmcs session client for raw-ecrt hybrid reference tools. CAN data
// stays in the fixed PDO region; this helper uses the 16-byte ARQ region only
// for SESSION_START and periodic KEEPALIVE packets required by production
// firmware's command gate.
class HybridSession {
public:
    using Clock = std::chrono::steady_clock;

    void begin() {
        endpoint_.reset();
        transmit_.clear();
        receive_.clear();
        established_ = false;
        queue_session(kSessionStart);
    }

    void prepare_output(std::uint8_t* process_image, Clock::time_point now) {
        if (established_ && now >= next_keepalive_) {
            queue_session(kSessionKeepalive);
            next_keepalive_ = now + kKeepaliveInterval;
        }
        endpoint_.build_own_chunk(
            reinterpret_cast<std::byte*>(process_image + ecat::kHybridStreamRegionOffset),
            transmit_);
    }

    void consume_input(const std::uint8_t* process_image, Clock::time_point now) {
        endpoint_.on_peer_chunk(
            reinterpret_cast<const std::byte*>(
                process_image + ecat::kHybridStreamRegionOffset),
            receive_);

        while (receive_.readable() >= kSessionPacketSize) {
            std::array<std::byte, kSessionPacketSize> packet{};
            (void)receive_.pop(packet);
            const auto first = static_cast<std::uint8_t>(packet[0]);
            const auto second = static_cast<std::uint8_t>(packet[1]);
            if (first != kExtendedSessionField || (second & 0x0FU) != kSessionFieldHighBits
                || read_u32_le(packet.data() + 2) != kNonce) {
                continue;
            }

            const std::uint8_t type = second >> 4;
            if (type == kSessionStartAck) {
                established_ = true;
                next_keepalive_ = now + kKeepaliveInterval;
            }
        }
    }

    [[nodiscard]] bool established() const noexcept { return established_; }

private:
    class ByteQueue {
    public:
        bool try_push(std::span<const std::byte> data) noexcept {
            if (data.size() > storage_.size() - size_)
                return false;
            std::memcpy(storage_.data() + size_, data.data(), data.size());
            size_ += data.size();
            return true;
        }

        std::size_t pop(std::span<std::byte> destination) noexcept {
            const std::size_t count = std::min(size_, destination.size());
            if (count == 0)
                return 0;
            std::memcpy(destination.data(), storage_.data(), count);
            std::memmove(storage_.data(), storage_.data() + count, size_ - count);
            size_ -= count;
            return count;
        }

        [[nodiscard]] std::size_t readable() const noexcept { return size_; }
        void clear() noexcept { size_ = 0; }

    private:
        std::array<std::byte, 256> storage_{};
        std::size_t size_ = 0;
    };

    static std::uint32_t read_u32_le(const std::byte* source) noexcept {
        return static_cast<std::uint32_t>(source[0])
             | static_cast<std::uint32_t>(source[1]) << 8
             | static_cast<std::uint32_t>(source[2]) << 16
             | static_cast<std::uint32_t>(source[3]) << 24;
    }

    void queue_session(std::uint8_t type) {
        std::array<std::byte, kSessionPacketSize> packet{};
        packet[0] = static_cast<std::byte>(kExtendedSessionField);
        packet[1] = static_cast<std::byte>((type << 4) | kSessionFieldHighBits);
        packet[2] = static_cast<std::byte>(kNonce);
        packet[3] = static_cast<std::byte>(kNonce >> 8);
        packet[4] = static_cast<std::byte>(kNonce >> 16);
        packet[5] = static_cast<std::byte>(kNonce >> 24);
        (void)transmit_.try_push(packet);
    }

    static constexpr std::size_t kSessionPacketSize = 6;
    static constexpr std::uint8_t kExtendedSessionField =
        static_cast<std::uint8_t>(data::DataId::kExtend);
    static constexpr std::uint8_t kSessionFieldHighBits =
        static_cast<std::uint8_t>(data::DataId::kSession) >> 4;
    static constexpr std::uint8_t kSessionStart =
        static_cast<std::uint8_t>(data::SessionType::kStart);
    static constexpr std::uint8_t kSessionStartAck =
        static_cast<std::uint8_t>(data::SessionType::kStartAck);
    static constexpr std::uint8_t kSessionKeepalive =
        static_cast<std::uint8_t>(data::SessionType::kKeepalive);
    static constexpr std::uint32_t kNonce = 0x524D4353U;
    static constexpr auto kKeepaliveInterval = std::chrono::milliseconds{250};

    ecat::HybridPdStreamEndpoint endpoint_;
    ByteQueue transmit_;
    ByteQueue receive_;
    bool established_ = false;
    Clock::time_point next_keepalive_{};
};

} // namespace librmcs::host::transport::igh::reference
