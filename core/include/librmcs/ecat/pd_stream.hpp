#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Shared between the rmcs_board EtherCAT firmware (core0 PDO callbacks) and
// the host transports (soem.cpp / igh.cpp); both sides run the same symmetric
// algorithm.
namespace librmcs::ecat {

// Go-back-N ARQ framing that carries the librmcs byte stream over EtherCAT
// process data.
//
// EtherCAT SyncManager buffered (3-buffer) mode is latest-wins: a chunk
// written by one side may be overwritten before the peer ever reads it, and
// the same chunk may be read many times. That is fine for classic cyclic
// setpoints but corrupts a byte stream, so the stream needs an ARQ layer.
//
// Wire layout of one chunk (little-endian, identical in both directions and
// unchanged from the stop-and-wait predecessor):
//     offset 0: u8  seq   -- 0 = nothing sent yet; data chunks use 1..255,
//                            wrapping 255 -> 1 (0 is never reused)
//     offset 1: u8  ack   -- CUMULATIVE: last peer seq consumed in order
//                            (0 = none)
//     offset 2: u16 len   -- payload bytes in this chunk; 0 = idle chunk
//                            (seq keeps the last sent value)
//     offset 4: payload[kPdChunkPayloadSize]
//
// Window discipline (the difference from the previous stop-and-wait): the
// sender keeps up to kPdWindow chunks in flight and stages a NEW chunk every
// received frame instead of every ack round trip, doubling the sustained
// cadence to one chunk per frame period. The receiver consumes only the
// exact next sequence number and withholds the ack otherwise, so a dropped
// frame (or an image overwritten before it was read) stalls the ack and the
// sender falls back to retransmitting from the oldest unacknowledged chunk
// -- go-back-N. On the measured link the loss rate is ~0 (0 corrupt frames
// over millions of cycles), so the go-back path is cold.
//
// Two invariants make a window > 1 safe over a latest-wins image:
//  * One new data chunk per frame: staging is gated by a credit that only
//    on_peer_chunk() grants -- the frame that delivered the peer's image
//    also read our previous image (EtherCAT reads and writes process data in
//    the same pass), so replacing it cannot lose an unread chunk. Extra
//    build_own_chunk() calls between frames (the slave's doorbell/main-loop
//    republish) retransmit instead of advancing.
//  * In-order-only consumption with a cumulative ack: a duplicate (same
//    image read twice) and a gap (image overwritten after a dropped frame)
//    are both rejected by the same seq check, and the stalled ack tells the
//    sender where to resume.
//
// BOTH ENDS MUST RUN THE SAME PROTOCOL VERSION before trusting the link: an
// old stop-and-wait receiver accepts ANY seq change (no in-order check), so
// after a real frame loss the go-back retransmission would be double-consumed
// and a pipelined chunk silently skipped. Loss-free mixed-version interop
// works (retransmission only fires after kGoBackStallFrames of stalled acks,
// i.e. on actual loss), which keeps bring-up simple -- but do not ship mixed.
//
// Throughput is one chunk payload per poll cycle (was one per two cycles); a
// burst of N chunks now drains in ~N+1 cycles instead of 2N. Backpressure is
// unchanged: a full receive ring withholds the ack, the window fills, and
// the sender retransmits without staging new payload.
inline constexpr std::size_t kPdChunkHeaderSize = 4;
inline constexpr std::size_t kPdChunkPayloadSize = 44;
inline constexpr std::size_t kPdChunkSize = kPdChunkHeaderSize + kPdChunkPayloadSize;

// In-flight chunks per direction. 2 is exactly enough for one-chunk-per-frame
// cadence (the ack for chunk k returns while chunk k+1 is on the wire); a
// deeper window would only add RAM and go-back replay depth.
inline constexpr unsigned kPdWindow = 2;

// Frames without ack progress (while chunks are in flight and nothing new is
// being staged) before concluding a frame was lost and entering go-back
// retransmission. The normal ack round trip is 2 frames.
inline constexpr unsigned kGoBackStallFrames = 4;

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
        head_ = 0;
        count_ = 0;
        send_offset_ = 0;
        stall_ = 0;
        may_stage_ = true;
    }

    // Feed one just-received image of the peer's chunk (slave: the RxPDO
    // outputs written by the master). Safe to call with the same image any
    // number of times. receive_ring must be the local end of the stream sink
    // (slave: the cross-core ring towards the fieldbus core).
    template <typename Ring>
    void on_peer_chunk(const std::byte* pd, Ring& receive_ring) noexcept {
        peer_ack_ = static_cast<std::uint8_t>(pd[1]);
        // The frame that carried this image also read our previous image, so
        // the next build may safely stage one new chunk (see file header).
        may_stage_ = true;

        const auto seq = static_cast<std::uint8_t>(pd[0]);
        std::uint16_t len;
        std::memcpy(&len, pd + 2, sizeof(len));

        if (seq == 0 || len == 0 || len > kPdChunkPayloadSize)
            return;
        if (seq != next_seq(rx_ack_))
            return; // duplicate or gap: withhold the ack, the peer goes back

        if (receive_ring.try_push({pd + kPdChunkHeaderSize, len}))
            rx_ack_ = seq; // consumed; echoed to the peer in build_own_chunk()
        // Ring full: the ack is withheld, so the peer keeps retransmitting --
        // backpressure without loss.
    }

    // True when build_own_chunk() could stage NEW payload right now: the
    // window has room (counting the chunks the latest cumulative ack already
    // covers) and a frame passed since the last staged chunk. Cheap peek for
    // out-of-cycle rebuild decisions (slave: refresh the ESC inputs as soon
    // as the fieldbus core delivers, instead of waiting for the next PDI
    // event); the authoritative check remains inside build_own_chunk().
    bool ready_to_advance() const noexcept {
        return may_stage_ && count_ - acked_prefix() < kPdWindow;
    }

    // (Re)build the local chunk image (slave: the TxPDO inputs read by the
    // master). Must write exactly kPdChunkSize bytes. transmit_ring is the
    // local stream source (slave: the cross-core ring from the fieldbus
    // core).
    template <typename Ring>
    void build_own_chunk(std::byte* pd, Ring& transmit_ring) noexcept {
        // Slide the window over everything the cumulative ack covers.
        const unsigned acked = acked_prefix();
        if (acked != 0) {
            head_ = (head_ + acked) % kPdWindow;
            count_ -= acked;
            send_offset_ = send_offset_ > acked ? send_offset_ - acked : 0;
            stall_ = 0;
        }

        const bool frame_credit = may_stage_;
        const Slot* out = nullptr;
        if (frame_credit && send_offset_ < count_) {
            // Go-back replay in progress: retransmit the window in order,
            // one chunk per frame, until it has all been resent.
            out = &slots_[(head_ + send_offset_) % kPdWindow];
            send_offset_++;
            may_stage_ = false;
        } else if (frame_credit && count_ < kPdWindow) {
            Slot& slot = slots_[(head_ + count_) % kPdWindow];
            slot.len = static_cast<std::uint16_t>(
                transmit_ring.pop({slot.payload, kPdChunkPayloadSize}));
            if (slot.len != 0) {
                tx_seq_ = next_seq(tx_seq_);
                slot.seq = tx_seq_;
                count_++;
                send_offset_ = count_; // the new chunk goes out right now
                may_stage_ = false;    // one new data chunk per frame
                out = &slot;
            }
        }
        if (out == nullptr && count_ != 0) {
            // Chunks in flight, everything sent, ack pending. An ack stalled
            // for several frames is the signature of a lost frame: rewind and
            // replay the whole window in order (see above) starting next
            // build. Otherwise keep the image painted with the NEWEST
            // transmitted chunk -- never an idle image: an extra build
            // between two frames (slave doorbell / main-loop republish) must
            // not replace a data image the peer has not read yet, and a
            // duplicate of the newest chunk is exactly the peer's last
            // consumed seq, which every receiver version ignores.
            if (frame_credit && ++stall_ >= kGoBackStallFrames) {
                send_offset_ = 0;
                stall_ = 0;
            } else if (send_offset_ != 0) {
                out = &slots_[(head_ + send_offset_ - 1) % kPdWindow];
            }
        }

        pd[1] = static_cast<std::byte>(rx_ack_);
        if (out != nullptr) {
            pd[0] = static_cast<std::byte>(out->seq);
            std::memcpy(pd + 2, &out->len, sizeof(out->len));
            std::memcpy(pd + kPdChunkHeaderSize, out->payload, out->len);
        } else {
            // Idle: everything acknowledged and nothing staged. Previous
            // seq, len = 0; the payload tail is left untouched.
            pd[0] = static_cast<std::byte>(tx_seq_);
            const std::uint16_t idle_len = 0;
            std::memcpy(pd + 2, &idle_len, sizeof(idle_len));
        }
    }

private:
    struct Slot {
        std::uint8_t seq = 0;
        std::uint16_t len = 0;
        // Retransmit staging: bytes popped from the ring must stay available
        // until acknowledged.
        std::byte payload[kPdChunkPayloadSize]{};
    };

    static constexpr std::uint8_t next_seq(std::uint8_t seq) noexcept {
        return seq == 255 ? std::uint8_t{1} : static_cast<std::uint8_t>(seq + 1);
    }

    // How many of the oldest in-flight chunks the cumulative ack covers.
    unsigned acked_prefix() const noexcept {
        for (unsigned j = 0; j < count_; ++j)
            if (slots_[(head_ + j) % kPdWindow].seq == peer_ack_)
                return j + 1;
        return 0;
    }

    std::uint8_t rx_ack_ = 0;   // last peer seq consumed in order (echoed as ack)
    std::uint8_t tx_seq_ = 0;   // seq of the newest staged own chunk (0 = none)
    std::uint8_t peer_ack_ = 0; // last own seq the peer reported consuming
    unsigned head_ = 0;         // slots_ index of the oldest unacknowledged chunk
    unsigned count_ = 0;        // in-flight chunks in [head_, head_ + count_)
    unsigned send_offset_ = 0;  // in-flight chunks already (re)transmitted
    unsigned stall_ = 0;        // frames without ack progress (go-back trigger)
    bool may_stage_ = true;     // one-new-chunk-per-frame credit
    Slot slots_[kPdWindow];
};

} // namespace librmcs::ecat
