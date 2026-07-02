#include "firmware/rmcs_board/app/src/usb/vendor.hpp"

#include <cstddef>
#include <cstdint>

#include <common/tusb_types.h>
#include <device/usbd.h>
#include <hpm_interrupt.h>

#include "core/src/protocol/serializer.hpp"
#include "firmware/rmcs_board/app/src/utility/boot_mailbox.hpp"

namespace librmcs::firmware::usb {

core::protocol::Serializer& get_serializer() { return vendor->serializer(); }

// TinyUSB device callbacks
extern "C" {

// USB0 interrupt vector. The HPM SDK leaves the concrete ISR in the example
// family.c (which this project does not build), so bind it here in app code
// instead of patching the SDK's dcd_hpm.c -- this keeps the hpm_sdk submodule
// pristine across version bumps. dcd_int_handler is tinyusb's device ISR entry.
void dcd_int_handler(uint8_t rhport);

SDK_DECLARE_EXT_ISR_M(IRQn_USB0, rmcs_usb0_isr)
void rmcs_usb0_isr(void) { dcd_int_handler(0); }

void tud_vendor_rx_cb(uint8_t itf, const uint8_t* buffer, uint16_t size) {
    if (itf != 0) [[unlikely]]
        return;

    const std::size_t max_packet_size = (tud_speed_get() == TUSB_SPEED_HIGH) ? 512 : 64;
    usb::vendor->handle_downlink(
        {reinterpret_cast<const std::byte*>(buffer), size}, size < max_packet_size);
}

void tud_dfu_runtime_reboot_to_dfu_cb() { boot::BootMailbox::reboot_to_bootloader(); }

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    usb::vendor->deactivate_session();
    usb::vendor->finish_downlink_transfer();
}

void tud_resume_cb() {}

void tud_mount_cb() {}

void tud_umount_cb() {
    usb::vendor->deactivate_session();
    usb::vendor->finish_downlink_transfer();
}

} // extern "C"

} // namespace librmcs::firmware::usb
