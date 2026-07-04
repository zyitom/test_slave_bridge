#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Shared between the rmcs_board EtherCAT firmware (core0 PDO callbacks) and
// the host SOEM transport; both sides run the same symmetric algorithm.
namespace librmcs::ecat {

// Stop-and-wait ARQ framing that carries the librmcs byte stream over
// EtherCAT process data.
//
// EtherCAT SyncManager buffered (3-buffer) mode is latest-wins: a chunk
// written by one side may be overwritten before the peer ever reads it, and
// the same chunk may be read many times. That is fine for classic cyclic
// setpoints but corrupts a byte stream, so each direction carries exactly one
// in-flight chunk which is advanced only after the peer echoes its sequence
// number back. Retransmission is free: the sender simply keeps the same chunk
// in the process data image until it is acknowledged.
//
// Wire layout of one chunk (little-endian, identical in both directions):
//     offset 0: u8  seq   -- 0 = nothing sent yet; data chunks use 1..255,
//                            wrapping 255 -> 1 (0 is never reused)
//     offset 1: u8  ack   -- last peer seq successfully consumed (0 = none)
//     offset 2: u16 len   -- payload bytes in this chunk; 0 = idle chunk
//                            (seq keeps the last sent value)
//     offset 4: payload[kPdChunkPayloadSize]
//
// A data chunk always has len > 0. The receiver treats a chunk as new when
// seq != 0, seq differs from the last consumed seq, and len > 0. Withholding
// the ack (because the local ring is full) makes the peer retransmit --
// natural end-to-end backpressure.
//
// Throughput is payload_size per poll and the ack path costs one poll, so
// with 124-byte payloads a 10 kHz master busy-poll moves ~1.2 MB/s per
// direction -- above four saturated classic 1 Mbit/s CAN buses.
inline constexpr std::size_t kPdChunkHeaderSize = 4;
inline constexpr std::size_t kPdChunkPayloadSize = 124;
inline constexpr std::size_t kPdChunkSize = kPdChunkHeaderSize + kPdChunkPayloadSize;

// One end of the stream. The slave instantiates it on core0 inside the SSC
// PDO-mapping callbacks; the master side runs the mirror image of the same
// algorithm (roles are fully symmetric).
class PdStreamEndpoint {
public:
    constexpr PdStreamEndpoint() = default;

    // Forget all link state. On the slave this runs on every SAFEOP -> OP
    // transition, paired with the master resetting its own endpoint before
    // requesting OP -- both sides restart from seq/ack 0.
    void reset() noexcept {
        rx_ack_ = 0;
        tx_seq_ = 0;
        peer_ack_ = 0;
        staged_len_ = 0;
    }

    // Feed one just-received image of the peer's chunk (slave: the RxPDO
    // outputs written by the master). Safe to call with the same image any
    // number of times. receive_ring must be the local end of the stream sink
    // (slave: the cross-core ring towards the fieldbus core).
    template <typename Ring>
    void on_peer_chunk(const std::byte* pd, Ring& receive_ring) noexcept {
        peer_ack_ = static_cast<std::uint8_t>(pd[1]);

        const auto seq = static_cast<std::uint8_t>(pd[0]);
        std::uint16_t len;
        std::memcpy(&len, pd + 2, sizeof(len));

        if (seq == 0 || seq == rx_ack_ || len == 0 || len > kPdChunkPayloadSize)
            return;

        if (receive_ring.try_push({pd + kPdChunkHeaderSize, len}))
            rx_ack_ = seq; // consumed; echoed to the peer in build_own_chunk()
        // Ring full: the ack is withheld, so the peer keeps the chunk in
        // flight and retries on every poll -- backpressure without loss.
    }

    // True when build_own_chunk() could stage NEW payload right now: nothing
    // sent yet, or the in-flight chunk has been acknowledged. Cheap peek for
    // out-of-cycle rebuild decisions (slave: refresh the ESC inputs as soon
    // as the fieldbus core delivers, instead of waiting for the next PDI
    // event); the authoritative check remains inside build_own_chunk().
    bool ready_to_advance() const noexcept { return tx_seq_ == 0 || peer_ack_ == tx_seq_; }

    // (Re)build the local chunk image (slave: the TxPDO inputs read by the
    // master). Must write exactly kPdChunkSize bytes. transmit_ring is the
    // local stream source (slave: the cross-core ring from the fieldbus core).
    template <typename Ring>
    void build_own_chunk(std::byte* pd, Ring& transmit_ring) noexcept {
        if (tx_seq_ == 0 || peer_ack_ == tx_seq_) {
            // Previous chunk acknowledged (or nothing sent yet): advance.
            staged_len_ =
                static_cast<std::uint16_t>(transmit_ring.pop({staging_, kPdChunkPayloadSize}));
            if (staged_len_ != 0)
                tx_seq_ = next_seq(tx_seq_);
            // Empty ring leaves an idle chunk: previous seq, len = 0.
        }
        // else: the in-flight chunk is unacknowledged; retransmit it as-is.

        pd[0] = static_cast<std::byte>(tx_seq_);
        pd[1] = static_cast<std::byte>(rx_ack_);
        std::memcpy(pd + 2, &staged_len_, sizeof(staged_len_));
        std::memcpy(pd + kPdChunkHeaderSize, staging_, staged_len_);
        // The payload tail beyond staged_len_ is left untouched; the header
        // length delimits the valid bytes.
    }

private:
    static constexpr std::uint8_t next_seq(std::uint8_t seq) noexcept {
        return seq == 255 ? std::uint8_t{1} : static_cast<std::uint8_t>(seq + 1);
    }

    std::uint8_t rx_ack_ = 0;   // last peer seq consumed (echoed as ack)
    std::uint8_t tx_seq_ = 0;   // seq of the currently staged own chunk (0 = none)
    std::uint8_t peer_ack_ = 0; // last own seq the peer reported consuming
    std::uint16_t staged_len_ = 0;
    // Retransmit staging: bytes popped from the ring must stay available
    // until acknowledged.
    std::byte staging_[kPdChunkPayloadSize]{};
};

} // namespace librmcs::ecat
