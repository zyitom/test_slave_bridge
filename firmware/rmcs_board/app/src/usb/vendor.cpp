#include "firmware/rmcs_board/app/src/usb/vendor.hpp"

#include <cstddef>
#include <cstdint>

#include <common/tusb_types.h>
#include <device/usbd.h>
#include <hpm_clock_drv.h>
#include <hpm_interrupt.h>
#include <hpm_mchtmr_drv.h>
#include <hpm_soc.h>

#include "core/src/protocol/serializer.hpp"
#include "firmware/rmcs_board/app/src/link/uplink.hpp"
#include "firmware/rmcs_board/app/src/utility/boot_mailbox.hpp"
#include "firmware/rmcs_board/app/src/xcore/pd_link.hpp"

namespace {

constexpr uint32_t kDfuRuntimeResetDelayMs = 50U;

volatile bool g_dfu_runtime_reboot_requested = false;
volatile uint32_t g_dfu_runtime_reboot_requested_ms = 0U;

uint32_t runtime_ms() {
    const uint64_t ticks_per_ms =
        static_cast<uint64_t>(clock_get_frequency(clock_mchtmr0)) / 1000U;
    return static_cast<uint32_t>(mchtmr_get_count(HPM_MCHTMR) / ticks_per_ms);
}

} // namespace

// Which transport owns the protocol stack. In the single-core image USB is the
// only one, so it binds the uplink hooks the CAN/UART ISRs serialize through.
//
// When the EtherCAT core1 image is present (LIBRMCS_APP_RELEASE_CORE1), the
// cross-core process-data link owns them instead and defines these in
// xcore/pd_link.cpp; USB keeps enumeration and DFU-RT but stays off the data
// plane. Two definitions would be a duplicate symbol, and leaving the USB
// downlink callback live would be worse than cosmetic: usb::Vendor is a second
// HostSession with its own deserializer, so a USB host could still open a
// session and drive CAN while EtherCAT believes it owns the link.
//
// Step 4 of the core swap merges both transports into one HostSession with two
// transmit backends and arbitration, at which point this #if goes away.
#if !(defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1)

namespace librmcs::firmware::link {

// The USB vendor class is the host transport of this application.
core::protocol::Serializer& uplink_serializer() { return usb::vendor->serializer(); }
bool uplink_enabled() { return usb::vendor->session_established(); }

} // namespace librmcs::firmware::link

#endif

namespace librmcs::firmware::usb {

void poll_dfu_runtime_reboot() {
    if (!g_dfu_runtime_reboot_requested)
        return;

    if ((runtime_ms() - g_dfu_runtime_reboot_requested_ms) < kDfuRuntimeResetDelayMs)
        return;

    boot::BootMailbox::reboot_to_bootloader();
}

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

#if defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1
    // Core-swap layout: the shared session lives in xcore::pd_link and USB is one
    // of its two backends. Traffic on the OUT endpoint is what claims the data
    // plane, but the handover runs in the main loop (it clears the batch pool,
    // which must not happen in interrupt context) -- so record the claim, and
    // only feed the deserializer once ownership has actually transferred.
    // Otherwise these bytes would interleave with the EtherCAT byte stream.
    xcore::notify_usb_activity();
    if (xcore::usb_owns_data_plane())
        xcore::pd_link->handle_usb_downlink(
            {reinterpret_cast<const std::byte*>(buffer), size}, size < max_packet_size);
#else
    usb::vendor->handle_downlink(
        {reinterpret_cast<const std::byte*>(buffer), size}, size < max_packet_size);
#endif
}

void tud_dfu_runtime_reboot_to_dfu_cb() {
    boot::BootMailbox::request_enter_dfu();
    g_dfu_runtime_reboot_requested_ms = runtime_ms();
    g_dfu_runtime_reboot_requested = true;
}

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
