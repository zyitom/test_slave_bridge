#include "firmware/rmcs_board/app/src/usb/vendor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <common/tusb_types.h>
#include <device/usbd.h>
#include <hpm_clock_drv.h>
#include <hpm_interrupt.h>
#include <hpm_mchtmr_drv.h>
#include <hpm_soc.h>

#include "core/src/protocol/serializer.hpp"
#include "firmware/rmcs_board/app/src/diag/can_diag.hpp"
#include "firmware/rmcs_board/app/src/link/uplink.hpp"
#include "firmware/rmcs_board/app/src/sync/sof.hpp"
#include "firmware/rmcs_board/app/src/utility/boot_mailbox.hpp"
#include "firmware/rmcs_board/app/src/xcore/pd_link.hpp"

namespace {

constexpr uint32_t kDfuRuntimeResetDelayMs = 50U;

// Landing pads for the receive callback's copy-then-arm sequence. Sized for one
// high-speed bulk packet, which is the most a single transfer can deliver here.
//
// WHY THE COPY EXISTS. The class driver hands the callback a pointer into the
// endpoint's own DMA buffer, and arming the endpoint again lets the controller
// start writing that same buffer. So the natural order is "process, then arm" --
// which leaves the endpoint un-armed for the whole processing time, and the
// device answers the host with NAK for all of it. Copying the packet out first
// removes that constraint: the endpoint can be armed immediately and the work
// runs against a private buffer.
//
// The measured cost of NOT doing this is large, because the per-packet budget is
// dominated by it: cycle = 1.2..1.4 x turnaround + ~13.5 us (rmcs_board/AGENTS.md),
// and the processing this skips ahead of is ~3 us of that turnaround.
//
// One pad per pipe, and no locking: both callbacks run from tud_task() on the
// main loop, and each consumes its pad before returning.
alignas(4) uint8_t g_downlink_copy[512];

// Bulk endpoint size, refreshed at mount. tud_vendor_rx_cb() below needs it to
// decide whether a packet is short (and therefore ends the transfer), and that
// callback runs inside the USB ISR once per downlink packet -- which made
// tud_speed_get() a jal/ret around one byte load on the hottest path there is.
// It lives in usbd.c, so the call could not be inlined away.
//
// Mount is the right place and session activation is NOT: the session-open
// packet arrives through tud_vendor_rx_cb itself, so a value refreshed on
// activation would still be the default while that packet was being classified.
// tud_mount_cb runs on SET_CONFIGURATION, before the endpoint exists and
// therefore before any packet can arrive on it, and the speed is fixed by the
// enumeration that just finished.
std::size_t g_packet_size = 64;
alignas(4) uint8_t g_can_downlink_copy[512];

volatile bool g_dfu_runtime_reboot_requested = false;
volatile uint32_t g_dfu_runtime_reboot_requested_ms = 0U;

uint32_t runtime_ms() {
    const uint64_t ticks_per_ms = static_cast<uint64_t>(clock_get_frequency(clock_mchtmr0)) / 1000U;
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

core::protocol::Serializer& can_uplink_serializer() {
#if LIBRMCS_SPLIT_CAN_ENDPOINT
    return usb::vendor->can_serializer();
#else
    return usb::vendor->serializer();
#endif
}

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

// Copy the packet out and re-arm BEFORE processing it, instead of processing
// first. Halves the window in which the endpoint has no buffer and NAKs the host
// (turnaround 2.22 -> 1.27 us, measured), at the cost of one memcpy per packet.
//
// DEFAULT OFF, because it buys nothing here: this host schedules exactly 8 bulk
// transactions per 125 us microframe per device (64000 packets/s, +-0.01% over
// six runs), and the device is already idle waiting when the next one arrives.
// Shrinking turnaround just moves the time into that idle wait -- measured
// 63999/63996/64002 vs 63988/63988/63991, i.e. identical.
//
// Kept switchable rather than deleted: on a host that schedules more than 8 per
// microframe the device would become the constraint and this would start to pay.
#ifndef LIBRMCS_COPY_THEN_ARM
# define LIBRMCS_COPY_THEN_ARM 0
#endif

// TinyUSB device callbacks
extern "C" {

// USB0 interrupt vector. The HPM SDK leaves the concrete ISR in the example
// family.c (which this project does not build), so bind it here in app code and
// keep both third-party submodules pristine. dcd_int_handler is TinyUSB's device
// ISR entry.
SDK_DECLARE_EXT_ISR_M(IRQn_USB0, rmcs_usb0_isr)
void rmcs_usb0_isr(void) {
    // Ahead of the device stack, because the whole point of the SOF probe is to
    // read FRINDEX at the earliest instant software can. A no-op that the
    // compiler removes entirely unless LIBRMCS_APP_SOF_DIAG is set.
    sync::sof_isr_entry();
    dcd_int_handler(0);
}

void tud_vendor_rx_cb(uint8_t itf, const uint8_t* buffer, uint16_t size) {
    const bool finished = size < g_packet_size;
    const uint16_t copy_size = std::min<uint16_t>(size, sizeof(g_downlink_copy));

#if LIBRMCS_SPLIT_CAN_ENDPOINT
    if (itf == 1) {
        // Copy, arm, then process -- see g_can_downlink_copy above. The arm has
        // to happen before the deserializer runs, which is the whole point; the
        // throttle it consults is refreshed from the main loop, so it does not
        // need this packet's CAN enqueues to have happened yet.
# if LIBRMCS_COPY_THEN_ARM
        std::memcpy(g_can_downlink_copy, buffer, copy_size);
        usb::vendor->poll_can_downlink_arm();
        usb::vendor->handle_can_downlink(
            {reinterpret_cast<const std::byte*>(g_can_downlink_copy), copy_size}, finished);
# else
        usb::vendor->handle_can_downlink(
            {reinterpret_cast<const std::byte*>(buffer), copy_size}, finished);
        usb::vendor->poll_can_downlink_arm();
# endif
        return;
    }
#endif

    if (itf != 0) [[unlikely]]
        return;

    // Timestamp before any work, so the turnaround this opens covers the whole
    // device-side path -- copy, re-arm. Compiled out unless LIBRMCS_APP_CAN_DIAG.
    diag::note_usb_out_complete();

#if LIBRMCS_COPY_THEN_ARM
    std::memcpy(g_downlink_copy, buffer, copy_size);
    usb::vendor->poll_downlink_arm();

    // Closes the turnaround and opens the starve interval. Everything below runs
    // against the private copy, with the endpoint already armed, so it is off the
    // per-packet critical path entirely.
    diag::note_usb_out_armed();
#else
    // Original ordering: the endpoint stays un-armed for the whole of the
    // processing below, because that processing reads the endpoint's own buffer.
    const uint8_t* const g_downlink_copy = buffer;
#endif

    const std::size_t max_packet_size = g_packet_size;
    (void)max_packet_size;

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
            {reinterpret_cast<const std::byte*>(g_downlink_copy), copy_size}, finished);
#else
    usb::vendor->handle_downlink(
        {reinterpret_cast<const std::byte*>(g_downlink_copy), copy_size}, finished);
#endif

#if !LIBRMCS_COPY_THEN_ARM
    usb::vendor->poll_downlink_arm();
    diag::note_usb_out_armed();
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

void tud_mount_cb() { g_packet_size = (tud_speed_get() == TUSB_SPEED_HIGH) ? 512U : 64U; }

void tud_umount_cb() {
    usb::vendor->deactivate_session();
    usb::vendor->finish_downlink_transfer();
}

} // extern "C"

} // namespace librmcs::firmware::usb
