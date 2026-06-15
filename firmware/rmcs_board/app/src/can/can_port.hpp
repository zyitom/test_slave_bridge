#pragma once

#include <cstdint>

namespace librmcs::firmware::board {

enum class CanMode : uint8_t {
    kClassic, // Classic CAN 2.0, 1Mbps
    kCanFd,   // CAN-FD, 1Mbps arbitration / 5Mbps data phase (BRS on)
};

// One physical CAN controller exposed by a board, in logical order (the first
// entry is CAN0, the second CAN1, ...). A board lists its ports in board_app.hpp;
// the shared CAN layer builds everything from that table, so there are no
// per-port macros.
struct CanPort {
    uint32_t base;    // HPM_MCANx_BASE
    uint32_t irq_num; // IRQn_MCANx
    CanMode mode;
};

} // namespace librmcs::firmware::board
