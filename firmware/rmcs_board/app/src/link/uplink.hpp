#pragma once

#include "core/src/protocol/serializer.hpp"

namespace librmcs::firmware::link {

// Uplink serializer of the active host transport (USB vendor class on
// rmcs_board, the EtherCAT process data stream on the ECAT bridge). Declared
// transport-neutrally so the CAN/UART drivers can be reused by any transport
// application; the application that owns the transport defines it.
core::protocol::Serializer& uplink_serializer();

} // namespace librmcs::firmware::link
