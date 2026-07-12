#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include <class/vendor/vendor_device.h>
#include <common/tusb_types.h>
#include <device/usbd.h>
#include <tusb.h>

#include "board_app.hpp"
#include "core/src/utility/assert.hpp"
#include "firmware/rmcs_board/app/src/link/host_session.hpp"
#include "firmware/rmcs_board/app/src/usb/usb_descriptors.hpp"
#include "firmware/rmcs_board/app/src/utility/lazy.hpp"

namespace librmcs::firmware::usb {

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
            core::utility::assert_debug(tud_vendor_n_write_zlp(0));
        }

        transmitted_size_ += target_size;
        if (transmitted_size_ == data.size() && target_size < max_packet_size) {
            finish_batch();
            transmitted_size_ = 0;
        }

        return true;
    }

private:
    void session_activated_callback() override { transmitted_size_ = 0; }

    size_t transmitted_size_ = 0;
};

inline constinit Vendor::Lazy vendor;

} // namespace librmcs::firmware::usb
