#include "firmware/ch32_board/app/src/usb/vendor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "ch32h417_usbss_device.h"
}

#include "core/src/protocol/serializer.hpp"

namespace librmcs::firmware::usb {

core::protocol::Serializer& get_serializer() { return vendor->serializer(); }

namespace ss {

namespace {
// Uplink TX staging buffer for bulk IN endpoint 1, aligned for the USBSS DMA.
__attribute__((aligned(4))) uint8_t tx_packet_buffer[Vendor::kMaxPacketSize];

// Set when an IN packet is armed on EP1, cleared by the EP1 IN-complete ISR hook
// below. volatile: shared between the forwarding loop and the USBSS interrupt.
volatile bool tx_in_flight = false;
} // namespace

bool enumerated() { return USBSS_DevEnumStatus != 0; }

bool tx_ready() { return enumerated() && !tx_in_flight; }

// Arm one bulk-IN packet on EP1. The (DMA, CHAIN_LEN, CHAIN_EXP_NUMP) sequence
// mirrors the demo's real EP3 IN arming in USBSS_Device_Endp_Init(); EXP_NUMP is
// the SuperSpeed burst count (packets of up to kMaxPacketSize).
bool tx_write(const uint8_t* data, size_t size) {
    if (size > sizeof(tx_packet_buffer))
        return false;
    std::memcpy(tx_packet_buffer, data, size);

    tx_in_flight = true;
    USBSSD->EP1_TX.UEP_TX_DMA = reinterpret_cast<uint32_t>(tx_packet_buffer);
    USBSSD->EP1_TX.UEP_TX_CHAIN_LEN = static_cast<uint16_t>(size);
    USBSSD->EP1_TX.UEP_TX_CHAIN_EXP_NUMP =
        static_cast<uint8_t>((size + Vendor::kMaxPacketSize - 1) / Vendor::kMaxPacketSize);
    return true;
}

bool tx_write_zlp() {
    tx_in_flight = true;
    USBSSD->EP1_TX.UEP_TX_DMA = reinterpret_cast<uint32_t>(tx_packet_buffer);
    USBSSD->EP1_TX.UEP_TX_CHAIN_LEN = 0;
    USBSSD->EP1_TX.UEP_TX_CHAIN_EXP_NUMP = 1;
    return true;
}

} // namespace ss

// ---- USBSS interrupt hooks (called from patched ch32h417_usbss_it.c) ---------
// These replace the CH372 demo's EP1 hardware DMA loopback with genuine librmcs
// endpoint servicing. See bsp/PROVENANCE.md for the vendored-ISR patch record.

extern "C" {

// EP1 bulk IN (device -> host uplink) transfer complete: ack the chain and free
// the endpoint so the forwarding loop can arm the next batch.
void usb_ss_ep1_in_complete(void) {
    USBSSD->EP1_TX.UEP_TX_CHAIN_ST |= USBSS_EP_TX_CHAIN_IF;
    ss::tx_in_flight = false;
}

// EP1 bulk OUT (host -> device downlink) transfer complete: hand the received
// bytes to the deserializer, then re-arm the RX chain.
//
// NOTE (bring-up): the received length is read from UEP_RX_CHAIN_LEN and the
// payload is taken from the RX buffer base (single-buffer, no ping-pong). Confirm
// the exact length/offset semantics of the chained-DMA RX on target before
// trusting multi-packet bursts.
void usb_ss_ep1_out_complete(void) {
    const uint32_t size = USBSSD->EP1_RX.UEP_RX_CHAIN_LEN;
    vendor->handle_downlink(
        {reinterpret_cast<const std::byte*>(USBSS_EP1_Rx_Buf), size},
        size < Vendor::kMaxPacketSize);

    USBSSD->EP1_RX.UEP_RX_DMA = reinterpret_cast<uint32_t>(USBSS_EP1_Rx_Buf);
    USBSSD->EP1_RX.UEP_RX_CHAIN_MAX_NUMP = DEF_ENDP1_OUT_BURST_LEVEL;
    USBSSD->EP1_RX.UEP_RX_CHAIN_ST |= USBSS_EP_RX_CHAIN_IF;
}

} // extern "C"

} // namespace librmcs::firmware::usb
