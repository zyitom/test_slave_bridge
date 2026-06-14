#include "firmware/mc02/app/src/usb/vendor.hpp"

#include <cstddef>
#include <cstdint>

#include <main.h>

#include "core/src/protocol/serializer.hpp"
#include "firmware/mc02/app/src/utility/boot_mailbox.hpp"

namespace librmcs::firmware::usb {

core::protocol::Serializer& get_serializer() { return vendor->serializer(); }

// TinyUSB device callbacks
extern "C" {

static volatile bool s_dfu_reboot_pending = false;

void tud_vendor_rx_cb(uint8_t itf, const uint8_t* buffer, uint16_t size) {
    if (itf != 0) [[unlikely]]
        return;

    usb::vendor->handle_downlink(
        {reinterpret_cast<const std::byte*>(buffer), size}, size < Vendor::kMaxPacketSize);
}

void tud_dfu_runtime_reboot_to_dfu_cb() {
    utility::boot_mailbox.request_enter_dfu();
    __DSB();
    __ISB();
    // TinyUSB calls this callback from CONTROL_STAGE_SETUP, immediately after
    // tud_control_status() queues the ZLP, before it is physically transmitted.
    // On STM32H723, calling NVIC_SystemReset() here corrupts the mailbox write
    // in the window before reset propagates (unlike STM32F4 where direct reset
    // works). Defer the reset to the main loop so the ZLP completes first.
    s_dfu_reboot_pending = true;
}

void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }

void tud_resume_cb() {}

void tud_mount_cb() {}

void tud_umount_cb() {}

} // extern "C"

bool dfu_reboot_pending() { return s_dfu_reboot_pending; }

[[noreturn]] void perform_dfu_reboot() {
    __DSB();
    __ISB();
    NVIC_SystemReset();
}

} // namespace librmcs::firmware::usb
