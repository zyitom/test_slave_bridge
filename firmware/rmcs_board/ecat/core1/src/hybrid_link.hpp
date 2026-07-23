#pragma once

#include "xcore_channel.hpp"

namespace librmcs::firmware::ecat {

// Hybrid fixed-PDO variant: give the CAN RX ISR (app/src/can/can.cpp) access to
// the cross-core mailbox uplink ring and the core0 doorbell. Must be called on
// core1 after the SHARE_RAM channel is available and BEFORE CAN interrupts can
// fire, so the ISR-side link::hybrid_can_uplink() hook has a live channel.
void hybrid_link_init(XcoreChannel& channel);

} // namespace librmcs::firmware::ecat
