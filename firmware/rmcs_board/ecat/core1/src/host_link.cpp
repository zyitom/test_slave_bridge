#include "host_link.hpp"

#include "core/src/protocol/serializer.hpp"
#include "firmware/rmcs_board/app/src/link/uplink.hpp"

namespace librmcs::firmware::link {

// The EtherCAT PD stream (via HostLink) is the host transport of this
// application; the CAN/UART driver ISRs serialize uplink data through it.
core::protocol::Serializer& uplink_serializer() { return ecat::host_link->serializer(); }

} // namespace librmcs::firmware::link
