#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(LIBRMCS_APP_USB_FULL_SPEED) && LIBRMCS_APP_USB_FULL_SPEED
# include <hpm_soc.h>
#endif

#include <class/vendor/vendor_device.h>
#include <common/tusb_types.h>
#include <device/usbd.h>
#include <tusb.h>

#include "board_app.hpp"
#include "core/src/utility/assert.hpp"
#include "firmware/rmcs_board/app/src/can/can.hpp"
#include "firmware/rmcs_board/app/src/link/host_session.hpp"
#include "firmware/rmcs_board/app/src/timer/timer.hpp"
#include "firmware/rmcs_board/app/src/usb/usb_descriptors.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::usb {

void poll_dfu_runtime_reboot();

// USB vendor-class host transport: TinyUSB bring-up plus the transmission
// shape (max-packet chunking + ZLP termination). Session lifecycle and
// downlink dispatch live in the shared link::HostSession.
class Vendor : public link::HostSession {
public:
    using Lazy = utility::Lazy<Vendor>;

    Vendor() {
        usb::usb_descriptors.init();

        const tusb_rhport_init_t init_config{
            .role = TUSB_ROLE_DEVICE,
            .speed = board::usb_use_high_speed() ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,
        };
        core::utility::assert_always(tusb_rhport_init(0, &init_config));

#if defined(LIBRMCS_APP_USB_FULL_SPEED) && LIBRMCS_APP_USB_FULL_SPEED
        // The speed field above is advisory only: the ChipIdea device driver
        // reports whatever the port negotiated and never forces it (grep
        // dcd_ci_hs.c -- it only READS PORTSC1_PORT_SPEED). Forcing full speed
        // is a controller bit: PFSC disables the high-speed chirp so the port
        // can only ever come up at 12 Mbit. Set after tusb_rhport_init, because
        // dcd_init resets the controller and would clear it. [RM: PORTSC1.PFSC]
        HPM_USB0->PORTSC1 |= USB_PORTSC1_PFSC_MASK;
#endif

        // tusb_rhport_init -> dcd_init already enabled the USB IRQ; pin its
        // priority explicitly so the CAN(3) > USB(2) > UART(1) preemption
        // hierarchy is owned here and cannot silently regress if the SDK default
        // changes. With preemptive interrupts on, this lets a CAN RX (3) preempt
        // a running USB ISR (2) -- keeping motor feedback off the USB ISR's tail.
        intc_m_enable_irq_with_priority(IRQn_USB0, 2);
    }

    // USB transfers have boundaries (unlike the EtherCAT PD stream): a short
    // packet finishes the transfer, so framing recovery can resynchronize.
    void handle_downlink(std::span<const std::byte> buffer, bool finished) {
        link::HostSession::handle_downlink(buffer);
        if (finished)
            finish_downlink_transfer();
    }

    // Re-arm the bulk OUT endpoint, unless the CAN transmit queues are too close
    // to full to absorb another packet. Runs once per main-loop pass.
    //
    // WHY THIS IS THROTTLED AT ALL. Left to itself the class driver re-arms the
    // endpoint the moment tud_vendor_rx_cb() returns, so a board whose software
    // queue is full keeps accepting frames it can only drop: the loss is silent
    // and the host never learns to slow down. Leaving the endpoint un-armed
    // makes the controller NAK instead, which stalls the host's in-flight URBs
    // and, once its 64-transfer pool is exhausted, blocks the sender inside
    // acquire_transmit_buffer(). That is the only backpressure signal this
    // protocol has -- there is no flow-control field on the wire.
    //
    // WHY IT IS NOT A NO-LOSS GUARANTEE. One 512-byte OUT packet holds up to ~46
    // eight-byte CAN records (11 bytes each), or ~170 DLC-0 ones, against a
    // 64-deep queue -- and up to 64 packets are already in flight by the time the
    // queue starts filling. This bounds the sustained rate; it cannot make
    // overflow impossible. Sizing the watermark for a worst-case packet would
    // mean throttling below a quarter of the queue and would cost throughput on
    // every normal burst.
    // Compiles away entirely with CFG_TUD_VENDOR_RX_MANUAL_XFER=0, where the
    // class driver re-arms the endpoint itself and there is no backpressure.
    // Keeping both paths alive makes the feature a one-macro A/B rather than a
    // revert, which is how its cost on the packet rate gets measured at all.
    void poll_downlink_arm() {
#if CFG_TUD_VENDOR_RX_MANUAL_XFER
        // Only when this pipe is the one carrying CAN. With the split on it
        // carries UART, config and session control -- none of which feed the CAN
        // transmit queue, so throttling here would stall traffic it does not
        // protect, including the session keepalive.
        if (!LIBRMCS_SPLIT_CAN_ENDPOINT && downlink_throttled()) {
            arm_pending_ = true;
            return;
        }
        // Keep the debt outstanding if the endpoint would not take the transfer
        // (not open yet, or one already in flight); the main-loop hook below then
        // retries. This is also what performs the very first arm, which manual
        // mode leaves to the application and nothing else would do.
        arm_pending_ = !tud_vendor_n_read_xfer(0);
#endif
    }

    // Main-loop hook. Two loads and a return unless an arm is actually owed --
    // which, in steady state, it never is: the rx completion callback has already
    // re-armed. Calling the full policy from the main loop as well cost 2.3% of
    // the packet rate for nothing, because it queried the CAN queues a second
    // time on every packet. What still has to reach the main loop is the initial
    // arm before any packet has arrived, and the release once a throttled queue
    // drains -- neither of which has a completion callback to ride on.
    void poll_downlink_arm_if_pending() {
#if CFG_TUD_VENDOR_RX_MANUAL_XFER
        if (arm_pending_)
            poll_downlink_arm();
#endif
    }

#if LIBRMCS_SPLIT_CAN_ENDPOINT
    // Uplink serializer for CAN. Its own batch pool and its own endpoint, so a
    // UART batch can never occupy the pipe a CAN record is waiting for.
    core::protocol::Serializer& can_serializer() { return can_serializer_; }

    void handle_can_downlink(std::span<const std::byte> buffer, bool finished) {
        can_deserializer_.feed(buffer);
        if (finished)
            can_deserializer_.finish_transfer();
    }

    // Backpressure lives on whichever pipe carries CAN, which is this one once
    // the split is on. Leaving it unthrottled here was measured to undo the
    // feature completely: stress at 25000 f/s went from 0.0000% loss back to
    // 20.5%, because the frames simply arrived on a pipe that never said no.
    // The bulk pipe's throttle is correspondingly disabled below -- with CAN
    // gone from it there is no queue there left to protect.
    void poll_can_downlink_arm() {
        if (throttle_active_) {
            can_arm_pending_ = true;
            return;
        }
        can_arm_pending_ = !tud_vendor_n_read_xfer(1);
    }

    // Re-evaluates the policy off the hot path and caches the answer for the
    // receive callback above, which then costs one bool load. The policy works on
    // millisecond scales (a 20 ms escape, watermarks a burst needs milliseconds
    // to cross), so the staleness this introduces cannot change its decisions.
    //
    // Chosen on structure, NOT on a measurement: the idle control's p99 is
    // bimodal at roughly 105 or 117 us across repeats of one identical firmware,
    // which swamps any difference between this and evaluating per packet. Do not
    // read a latency win into this shape -- there is no evidence of one.
    void poll_can_downlink_arm_if_pending() {
        // Steady state is two bool loads and a return. The policy is re-evaluated
        // only when it is already engaged (so it can release), when an arm is
        // owed, or on a coarse tick -- every 16 passes is about 21 us, against a
        // queue that needs milliseconds to drain and watermarks with 16 slots of
        // headroom, so nothing can cross unnoticed. Evaluating it on every pass
        // would be ~770k queue walks a second for no benefit.
        if (can_arm_pending_ || throttle_active_ || (++throttle_tick_ & 0xFU) == 0U)
            throttle_active_ = downlink_throttled();
        if (can_arm_pending_)
            poll_can_downlink_arm();
    }

    // Same shape as try_transmit() below, on endpoint index 1 and the CAN batch
    // pool. Kept as a separate function rather than a parameterized one so the
    // single-pipe build compiles to byte-identical code.
    bool try_transmit_can() {
        if (!session_established())
            return false;

        if (!can_transmitting_batch_)
            can_transmitting_batch_ = can_transmit_buffer_.pop_batch();
        if (!can_transmitting_batch_)
            return false;

        if (!tud_vendor_n_write_available(1))
            return false;

        const auto data = can_transmitting_batch_->data();
        const std::size_t max_packet_size = (tud_speed_get() == TUSB_SPEED_HIGH) ? 512 : 64;
        const auto target_size = std::min(data.size() - can_transmitted_size_, max_packet_size);

        if (target_size) {
            const auto* src = reinterpret_cast<const uint8_t*>(data.data() + can_transmitted_size_);
            core::utility::assert_debug(
                tud_vendor_n_write(1, src, target_size) == target_size);
        } else {
            static constexpr uint8_t kZlpByte = 0;
            (void)tud_vendor_n_write(1, &kZlpByte, 0);
        }

        can_transmitted_size_ += target_size;
        if (can_transmitted_size_ == data.size() && target_size < max_packet_size) {
            link::InterruptSafeBuffer::release_batch(can_transmitting_batch_);
            can_transmitting_batch_ = nullptr;
            can_transmitted_size_ = 0;
        }

        return true;
    }
#endif

    bool try_transmit() {
        const auto* batch = next_batch();
        if (!batch)
            return false;

        if (!tud_vendor_n_write_available(0))
            return false;

        const auto data = batch->data();

        const std::size_t max_packet_size = (tud_speed_get() == TUSB_SPEED_HIGH) ? 512 : 64;
        const auto target_size = std::min(data.size() - transmitted_size_, max_packet_size);

        if (target_size) {
            const auto* src = reinterpret_cast<const uint8_t*>(data.data() + transmitted_size_);
            core::utility::assert_debug(tud_vendor_n_write(0, src, target_size) == target_size);
        } else {
            // TinyUSB 0.21 direct mode submits a ZLP through the normal write
            // API. Its return value is the transferred length, hence zero for
            // both a successful ZLP and an error; write_available() above has
            // already proved that the endpoint is mounted and unclaimed.
            static constexpr uint8_t kZlpByte = 0;
            (void)tud_vendor_n_write(0, &kZlpByte, 0);
        }

        transmitted_size_ += target_size;
        if (transmitted_size_ == data.size() && target_size < max_packet_size) {
            finish_batch();
            transmitted_size_ = 0;
        }

        return true;
    }

protected:
    void session_activated_callback() override {
        transmitted_size_ = 0;
#if LIBRMCS_SPLIT_CAN_ENDPOINT
        // The CAN pipe's pool is separate, so HostSession's own reset does not
        // reach it: a stale batch here would be transmitted into the new session
        // and decoded against the wrong stream.
        if (can_transmitting_batch_) {
            link::InterruptSafeBuffer::release_batch(can_transmitting_batch_);
            can_transmitting_batch_ = nullptr;
        }
        can_transmit_buffer_.clear();
        can_transmitted_size_ = 0;
        can_deserializer_.finish_transfer();
#endif
    }

private:
    // Hysteresis. Engage near the top of the queue, release only once it has
    // really drained: with a single threshold the endpoint would re-arm on the
    // pass after every single dequeue, and the throttle would chatter at exactly
    // the depth where latency is already worst.
    static constexpr size_t kThrottleEngageDepth = can::Can::kTransmitQueueSize * 3 / 4;
    static constexpr size_t kThrottleReleaseDepth = can::Can::kTransmitQueueSize / 4;

    // Escape hatch. A bus that has stopped draining -- bus-off, or simply no
    // other node to acknowledge -- would otherwise hold the OUT endpoint closed
    // forever. That endpoint also carries the UART downlink and the session
    // keepalive, so a fault on one CAN bus would take the whole link down with
    // it, which is far worse than dropping the frames aimed at that bus. After
    // this long with the queue still above the release watermark, stop
    // withholding. A healthy bus drains all 64 slots in ~3.2 ms at the measured
    // ~19.8k frames/s, so 20 ms only elapses when the bus really is stuck; the
    // session lease is 1000 ms, well clear of it.
    static constexpr uint64_t kThrottleDeadlineQuarterUs = 20'000U * 4U;

    bool downlink_throttled() {
        const size_t depth = can::max_transmit_queue_depth();

        // Backpressure was abandoned for this episode. Keep the pipe open until
        // the bus proves it is draining again -- re-engaging at the engage
        // watermark would just restart the same 20 ms stall in a loop.
        if (throttle_abandoned_) {
            if (depth <= kThrottleReleaseDepth)
                throttle_abandoned_ = false;
            return false;
        }

        if (!throttling_) {
            if (depth < kThrottleEngageDepth)
                return false;
            throttling_ = true;
            throttle_started_ = timer::Timer::timestamp64_quarter_us();
            return true;
        }

        if (depth <= kThrottleReleaseDepth) {
            throttling_ = false;
            return false;
        }

        if (timer::Timer::timestamp64_quarter_us() - throttle_started_
            >= kThrottleDeadlineQuarterUs) {
            throttling_ = false;
            throttle_abandoned_ = true;
            return false;
        }

        return true;
    }

    size_t transmitted_size_ = 0;

#if LIBRMCS_SPLIT_CAN_ENDPOINT
    link::InterruptSafeBuffer can_transmit_buffer_;
    core::protocol::Serializer can_serializer_{can_transmit_buffer_};
    core::protocol::Deserializer can_deserializer_{deserialize_callback()};
    const link::InterruptSafeBuffer::Batch* can_transmitting_batch_ = nullptr;
    size_t can_transmitted_size_ = 0;
    bool can_arm_pending_ = true;
#endif

    // Starts true: the endpoint has to be armed once before any packet can
    // arrive, and only the main-loop hook can do it.
    bool arm_pending_ = true;
    // Cached answer of downlink_throttled(), refreshed once per main-loop pass.
    bool throttle_active_ = false;
    uint32_t throttle_tick_ = 0;
    bool throttling_ = false;
    bool throttle_abandoned_ = false;
    uint64_t throttle_started_ = 0;
};

inline constinit Vendor::Lazy vendor;

} // namespace librmcs::firmware::usb
