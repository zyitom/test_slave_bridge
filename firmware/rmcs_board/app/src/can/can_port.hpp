#pragma once

#include <cstdint>

namespace librmcs::firmware::board {

enum class CanMode : uint8_t {
    kClassic, // Classic CAN 2.0, 1Mbps
    kCanFd,   // CAN-FD, 1Mbps arbitration / 5Mbps data phase (BRS on)
};

// One physical CAN controller exposed by a board, in port order: the first
// entry is the connector printed CAN1 (DataId::kCan1), the second CAN2, and so
// on -- board, protocol and host API all use that same 1-based number. A board
// lists its ports in board_app.hpp; the shared CAN layer builds everything from
// that table, so there are no per-port macros.
struct CanPort {
    uint32_t base;    // HPM_MCANx_BASE
    uint32_t irq_num; // IRQn_MCANx
    CanMode mode;
};

} // namespace librmcs::firmware::board
