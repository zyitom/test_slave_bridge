#pragma once

#include "core/src/protocol/serializer.hpp"

#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD
# include <cstddef>

# include "core/include/librmcs/data/datas.hpp"
#endif

namespace librmcs::firmware::link {

// Uplink serializer of the active host transport (USB vendor class on
// rmcs_board, the EtherCAT process data stream on the ECAT bridge). Declared
// transport-neutrally so the CAN/UART drivers can be reused by any transport
// application; the application that owns the transport defines it.
core::protocol::Serializer& uplink_serializer();
bool uplink_enabled();

#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD
// Hybrid fixed-PDO variant: raw CAN data frames bypass the protocol serializer
// (which the stream also uses) and are forwarded straight into the native
// mailbox uplink. The CAN RX ISR calls hybrid_can_uplink() once per received
// frame, then hybrid_uplink_notify() once to wake core0. Implemented by the
// EtherCAT fieldbus application (ecat/core1/src/hybrid_link.cpp).
bool hybrid_fixed_active();
bool hybrid_can_uplink(std::size_t bus, const data::CanDataView& data);
void hybrid_uplink_notify();
#endif

} // namespace librmcs::firmware::link
