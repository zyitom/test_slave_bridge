#pragma once

#include <cstdint>

// CAN port naming for the rmcs_board family.
//
// EVERY layer now uses the same 1-based number as the enclosure: the socket
// printed CAN1 is CanPort::kCan1 is DataId::kCan1 is the board's first
// controller. That is also what mc02 / c_board / ch32_board always did --
// their can1_transmit() has always written DataId::kCan1.
//
// rmcs_board was the one exception in the repository: its methods were
// can0_transmit()/can1_transmit() and they wrote DataId::kCan0/kCan1, so the
// socket printed CAN1 answered to "CAN0" in code and to "kCan0" on the wire.
// That cost a long hardware investigation on 2026-09-04 -- a physical-layer
// fault was chased all the way down to sampling a receive pin, on a port that
// had no cable in it, because both sides of the conversation said "CAN1" and
// meant different connectors.
//
// The old can0_transmit()/can1_transmit() methods are DELETED rather than
// deprecated, and that is deliberate. Keeping them would be worse than the
// ambiguity being removed: can1_transmit() exists in both schemes with
// opposite meanings, so an old call site would still compile and silently
// drive the other bus. Deleting them turns every such call site into a
// compile error.
//
// DataId::kCan0 is now unused by every board in the repository. It stays in the
// enum because its numeric value is part of the wire format and renumbering the
// ids after it would break every existing firmware.

namespace librmcs::board::rmcs {

enum class CanPort : uint8_t {
    kCan1 = 1, // socket "CAN1" -- first controller, DataId::kCan1
    kCan2 = 2,
    kCan3 = 3,
    kCan4 = 4,
};

} // namespace librmcs::board::rmcs
