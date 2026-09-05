#pragma once

// Compatibility header. The dual-CAN hpm5321 no longer has a class of its own:
// both PCBs run one firmware image and report their bus count over EP0, so
// RmcsBoardHpm5321 serves both and opens either product ID. See
// rmcs_board_hpm5321.hpp.
//
// The alias is not deprecated by attribute on purpose -- it names a real board
// and reads better than the generic name at call sites that only ever talk to
// the dual-CAN PCB.

#include <librmcs/board/rmcs_board_hpm5321.hpp>

namespace librmcs::board {

using RmcsBoardHpm5321DualCan = RmcsBoardHpm5321;

} // namespace librmcs::board
