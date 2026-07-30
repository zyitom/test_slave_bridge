// core0 USB device: a FULL DATA TRANSPORT, not just a firmware-update hook.
//
// Do not infer this file's scope from its name or from its first ~180 lines.
// "runtime" here names the DFU RUNTIME (DFU-RT) interface, and the DFU-RT code
// happens to come first -- but the second half of the file is the byte shuttle
// between the USB vendor bulk endpoints and the SHARE_RAM cross-core rings
// (see pump_vendor_data()). Reading only the header has twice produced the
// wrong conclusion that core0's USB is "DFU only, off the data path".
//
// Composite device = vendor (interface 0, bulk 0x01/0x81, HS 512B) + DFU-RT
// (interface 1). The vendor interface carries the same librmcs protocol byte
// stream that EtherCAT carries over the PDO, against the same pair of rings;
// the two host transports are arbitrated (one owner at a time, never
// concurrent) via rmcs_pd_set_usb_active() in pd_glue.cpp / hybrid_glue.cpp.
//
// Why USB shares core0 with the EtherCAT stack instead of living on core1 next
// to CAN: USB and EtherCAT are mutually exclusive, while USB and CAN are always
// concurrent (CAN is the far end of the same data path). Co-locating the
// mutually exclusive pair is what makes core0 contention-free by construction.
// Full argument, plus the rejected "USB on core1" alternative and the data
// needed to revisit it, in ../../DESIGN.md sections 3 and 3.1.

#include "usb_runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <tuple>

#include <board.h>
#include <class/dfu/dfu.h>
#include <common/tusb_types.h>
#include <device/dcd.h>
#include <device/usbd.h>
#include <hpm_clock_drv.h>
#include <hpm_interrupt.h>
#include <hpm_mchtmr_drv.h>
#include <hpm_otp_drv.h>
#include <hpm_soc.h>
#include <tusb.h>
#include <tusb_config.h>
#include <tusb_option.h>

#include "firmware/rmcs_board/common/boot_mailbox.hpp"
#include "rmcs_pd.h"

namespace {

constexpr uint8_t kUsbIrqPriority = 2U;
constexpr uint32_t kDfuRuntimeResetDelayMs = 50U;

constexpr size_t kUuidWordCount = OTP_SOC_UUID_LEN / sizeof(uint32_t);
static_assert((OTP_SOC_UUID_LEN % sizeof(uint32_t)) == 0);
static_assert(kUuidWordCount == 4U);

std::array<char, 43> g_serial_string{"EC-0000-0000-0000-0000-0000-0000-0000-0000"};
std::array<uint16_t, 128> g_descriptor_string_buffer{};
bool g_serial_string_ready = false;
volatile bool g_dfu_runtime_reboot_requested = false;
volatile uint32_t g_dfu_runtime_reboot_requested_ms = 0U;

constexpr tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xA11C,
    .idProduct = LIBRMCS_USB_PID,
    .bcdDevice = 0x0300,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// Composite device: a vendor (bulk) data interface plus the DFU runtime. The
// vendor interface is FIRST (number 0) so the host librmcs USB transport, which
// claims interface 0, binds it unchanged; the DFU runtime that reflashing uses
// stays available alongside it.
// NOLINTNEXTLINE(cppcoreguidelines-use-enum-class)
enum InterfaceNumber : uint8_t {
    kItfNumVendor = 0,
    kItfNumDfuRuntime,
    kItfNumTotal,
};

// Bulk data endpoints, STM32-HAL style (EP1 OUT / EP1 IN) -- identical to the
// standalone USB app so the host side is transport-agnostic. The HPM6E80 USB0
// PHY runs high speed, so the bulk max packet is 512.
constexpr uint8_t kEpnumDataOut = 0x01;
constexpr uint8_t kEpnumDataIn = 0x81;
constexpr uint16_t kBulkMaxPacket = 512;

constexpr size_t kConfigTotalLen =
    TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_DFU_RT_DESC_LEN;

constexpr uint8_t kConfigurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1, kItfNumTotal, 0, kConfigTotalLen, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_VENDOR_DESCRIPTOR(kItfNumVendor, 5, kEpnumDataOut, kEpnumDataIn, kBulkMaxPacket),
    TUD_DFU_RT_DESCRIPTOR(
        kItfNumDfuRuntime, 4, DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_WILL_DETACH, 1000, 1024),
};
static_assert(sizeof(kConfigurationDescriptor) == kConfigTotalLen);

constexpr std::array<uint8_t, 2> kLanguageId = {0x09, 0x04};
constexpr std::string_view kManufacturerString = "Alliance RoboMaster Team.";
constexpr std::string_view kProductString = "RMCS EtherCAT Bridge v" LIBRMCS_PROJECT_VERSION_STRING;
constexpr std::string_view kDfuRuntimeString = "DFU Runtime";
constexpr std::string_view kDataStreamString = "RMCS Data Stream";

constexpr uint32_t mix_step(uint32_t value) {
    value *= 0x9E3779B9U;
    return value ^ (value >> 16U);
}

void mix_uid_entropy(std::array<uint32_t, kUuidWordCount>& uid) {
    auto& [a, b, c, d] = uid;

    a ^= mix_step(b ^ c ^ d);
    b ^= mix_step(a ^ c ^ d);
    c ^= mix_step(a ^ b ^ d);
    d ^= mix_step(a ^ b ^ c);

    a ^= mix_step(b + c + d);
    b ^= mix_step(a + c + d);
    c ^= mix_step(a + b + d);
    d ^= mix_step(a + b + c);

    a ^= mix_step((b << 5U) ^ (c >> 3U) ^ d);
    b ^= mix_step((c << 7U) ^ (d >> 5U) ^ a);
    c ^= mix_step((d << 11U) ^ (a >> 7U) ^ b);
    d ^= mix_step((a << 13U) ^ (b >> 11U) ^ c);

    a += mix_step(b ^ d);
    b += mix_step(c ^ a);
    c += mix_step(d ^ b);
    d += mix_step(a ^ c);
}

char* write_hex_u16(uint16_t value, char* buffer) {
    static constexpr char kHexLut[] = "0123456789ABCDEF";

    *buffer++ = kHexLut[(value >> 12U) & 0xFU];
    *buffer++ = kHexLut[(value >> 8U) & 0xFU];
    *buffer++ = kHexLut[(value >> 4U) & 0xFU];
    *buffer++ = kHexLut[value & 0xFU];

    return buffer;
}

uint32_t runtime_ms() {
    const uint64_t ticks_per_ms =
        static_cast<uint64_t>(clock_get_frequency(clock_mchtmr0)) / 1000U;
    return static_cast<uint32_t>(mchtmr_get_count(HPM_MCHTMR) / ticks_per_ms);
}

void update_serial_string() {
    if (g_serial_string_ready) {
        return;
    }

    std::array<uint32_t, kUuidWordCount> uuid{};
    for (size_t i = 0; i < uuid.size(); ++i) {
        uuid[i] = otp_read_from_shadow(OTP_SOC_UUID_IDX + static_cast<uint32_t>(i));
    }

    mix_uid_entropy(uuid);

    char* cursor = g_serial_string.data() + 3;
    for (uint32_t word : uuid) {
        cursor = write_hex_u16(static_cast<uint16_t>(word >> 16U), cursor) + 1;
        cursor = write_hex_u16(static_cast<uint16_t>(word), cursor) + 1;
    }

    g_serial_string_ready = true;
}

void request_reboot_to_bootloader() {
    librmcs::firmware::boot::BootMailbox::request_enter_dfu();
    g_dfu_runtime_reboot_requested_ms = runtime_ms();
    g_dfu_runtime_reboot_requested = true;
    dcd_sof_enable(0, true);
}

[[noreturn]] void reboot_to_bootloader() {
    librmcs::firmware::boot::BootMailbox::reboot();
}

void poll_dfu_runtime_reboot() {
    if (!g_dfu_runtime_reboot_requested)
        return;

    if ((runtime_ms() - g_dfu_runtime_reboot_requested_ms) < kDfuRuntimeResetDelayMs)
        return;

    reboot_to_bootloader();
}

// Byte-shuttle between the USB bulk endpoints and the cross-core rings. Called
// from the USB ISR right after tud_task(), so every tinyusb FIFO access stays
// in one context (no reentrancy) and the rings keep a single core0-side
// producer/consumer. Only runs while USB owns the link (rmcs_pd_set_usb_active),
// which the ESC hooks honour by going inert -- so the ring invariants hold.
void pump_vendor_data() {
    if (!rmcs_pd_usb_active())
        return;

    // Host -> device: drain the bulk-OUT FIFO into the downlink ring, but only
    // as much as the ring can accept, so nothing read out of the FIFO is lost.
    for (;;) {
        const uint32_t available = tud_vendor_available();
        if (available == 0)
            break;
        const size_t room = rmcs_pd_downlink_free();
        if (room == 0)
            break; // Ring full: leave bytes in the FIFO -> USB backpressure.
        uint8_t buffer[kBulkMaxPacket];
        size_t want = available;
        if (want > room)
            want = room;
        if (want > sizeof(buffer))
            want = sizeof(buffer);
        const uint32_t got = tud_vendor_read(buffer, static_cast<uint32_t>(want));
        if (got == 0)
            break;
        rmcs_pd_push_downlink(buffer, got); // Fits by construction (got <= room).
    }

    // Device -> host: move uplink-ring bytes into the bulk-IN FIFO.
    bool wrote = false;
    for (;;) {
        const uint32_t writable = tud_vendor_write_available();
        if (writable == 0)
            break;
        uint8_t buffer[kBulkMaxPacket];
        size_t capacity = writable;
        if (capacity > sizeof(buffer))
            capacity = sizeof(buffer);
        const size_t got = rmcs_pd_pop_uplink(buffer, capacity);
        if (got == 0)
            break;
        (void)tud_vendor_write(buffer, static_cast<uint32_t>(got));
        wrote = true;
    }
    if (wrote)
        (void)tud_vendor_write_flush();
}

} // namespace

extern "C" {

void dcd_int_handler(uint8_t rhport);

SDK_DECLARE_EXT_ISR_M(IRQn_USB0, rmcs_usb0_isr)
void rmcs_usb0_isr(void) {
    dcd_int_handler(0);
    // Service the device stack in ISR context so DFU-runtime control transfers
    // (enumeration, SET_INTERFACE, detach-to-bootloader) always complete even if
    // the main loop is busy or stalled -- notably the SSC MainLoop, whose stalls
    // are the known cause of "Cannot set alternate interface: LIBUSB_ERROR_OTHER"
    // wedges that made reflashing require the KEYA force-bootloader dance.
    tud_task();
    poll_dfu_runtime_reboot();
    // NOTE: the vendor DATA pump does NOT run here. It runs only in
    // rmcs_usb_runtime_task() (main loop, with this IRQ masked), so the vendor
    // FIFOs and the cross-core rings have exactly ONE core0-side accessor -- a
    // second pump here raced this one at high throughput and desynced the byte
    // stream (host saw corrupt/misattributed frames). tud_task() still fills the
    // RX FIFO / drains the TX FIFO here; the masked main-loop pump moves bytes.
}

// Host started talking on the vendor OUT endpoint: claim the link for USB. The
// ESC hooks go inert (pd_glue) so the two transports never fight over the rings.
void tud_vendor_rx_cb(uint8_t itf, uint8_t const* buffer, uint16_t bufsize) {
    (void)itf;
    (void)buffer;
    (void)bufsize;
    rmcs_pd_set_usb_active(true);
}

// USB unplugged: hand the link back to the EtherCAT path.
void tud_umount_cb(void) { rmcs_pd_set_usb_active(false); }

uint8_t const* tud_descriptor_device_cb(void) {
    return reinterpret_cast<uint8_t const*>(&kDeviceDescriptor);
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return kConfigurationDescriptor;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t str_size = 0;
    if (index == 0U) {
        std::memcpy(&g_descriptor_string_buffer[1], kLanguageId.data(), kLanguageId.size());
        str_size = 1;
    } else {
        std::string_view str;
        switch (index) {
        case 1: str = kManufacturerString; break;
        case 2: str = kProductString; break;
        case 3:
            update_serial_string();
            str = std::string_view{g_serial_string.data(), g_serial_string.size() - 1U};
            break;
        case 4: str = kDfuRuntimeString; break;
        case 5: str = kDataStreamString; break;
        default: return nullptr;
        }

        constexpr size_t kMaxStringSize = std::min<size_t>(
            std::tuple_size_v<decltype(g_descriptor_string_buffer)> - 1U,
            (std::numeric_limits<uint8_t>::max() - 2U) / 2U);
        str_size = static_cast<uint8_t>(std::min<size_t>(str.size(), kMaxStringSize));

        for (uint8_t i = 0; i < str_size; ++i) {
            g_descriptor_string_buffer[i + 1U] = static_cast<uint16_t>(str[i]);
        }
    }

    g_descriptor_string_buffer[0] =
        (TUSB_DESC_STRING << 8U) | static_cast<uint16_t>((2U * str_size) + 2U);
    return g_descriptor_string_buffer.data();
}

void tud_dfu_runtime_reboot_to_dfu_cb(void) { request_reboot_to_bootloader(); }

void rmcs_usb_runtime_init(void) {
    board_init_usb();
    update_serial_string();

    const tusb_rhport_init_t init_config = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    (void)tusb_rhport_init(0, &init_config);
    intc_m_enable_irq_with_priority(IRQn_USB0, kUsbIrqPriority);
}

// tud_task() (enumeration/DFU) still runs entirely in rmcs_usb0_isr so a stalled
// main loop can never wedge the DFU runtime. This main-loop hook only drives the
// vendor DATA pump: when the host is waiting for a reply (e.g. SESSION_ACK) it
// sends no OUT traffic, so the USB ISR may not fire and the uplink would stall.
// Pumping here too keeps the uplink draining. Mask the USB IRQ around it so this
// thread-context pump never races tud_task()/the ISR-side pump on the vendor
// FIFOs (the ISR is the only other toucher; priority persists across the mask).
void rmcs_usb_runtime_task(void) {
    poll_dfu_runtime_reboot();
    intc_m_disable_irq(IRQn_USB0);
    pump_vendor_data();
    intc_m_enable_irq(IRQn_USB0);
    poll_dfu_runtime_reboot();
}

} // extern "C"
