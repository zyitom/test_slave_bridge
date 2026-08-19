#pragma once

// Direct cross-board skew measurement over the existing UART0 wires, using
// GPTMR hardware compare and capture -- no CPU in either the send or the
// receive instant.
//
// WHY THIS EXISTS. Every earlier attempt measured the timeline through the CAN
// timestamp unit, whose timebase is PTPC. PTPC hangs off PLL0, which the
// HPM5300 datasheet describes as fractional-N with spread-spectrum support, and
// its instantaneous rate wanders by hundreds of ppm. That wander -- not the
// timeline, and not the boards -- is what produced every bogus figure from
// 452 us down to 30 us.
//
// GPTMR is in CLK_SRC_GROUP_COMMON, so it can be clocked straight from the
// 24 MHz crystal, the same source the machine timer divides by 6. No PLL, no
// fractional divider, no spread spectrum, and an exact 3000 ticks per 125 us
// microframe. That is the whole reason to prefer it.
//
// WIRING: none to add. UART0 already crosses between the two boards
// (A.TXD<->B.RXD both ways), and those pins carry GPTMR0 channel 1's compare
// output and capture input as ALT1:
//
//   PB08  UART0 TXD  /  GPTMR0_COMP_1   -- pulse out
//   PB09  UART0 RXD  /  GPTMR0_CAPT_1   -- pulse in
//
// So the existing cable becomes a bidirectional hardware timing link. UART0 is
// unavailable while this is enabled, which is why it is its own build option.
//
// MEASUREMENT. Each board fires a pulse at an agreed microframe and captures
// the other's. Board A's pulse leaves at microframe a_tx and is captured at
// b_rx; board B's leaves at b_tx and is captured at a_rx. Then
//
//     skew = ((b_rx - a_tx) - (a_rx - b_tx)) / 2
//
// Cable propagation and the pad/synchroniser delay appear with the same sign in
// both directions and cancel in the difference -- the standard two-way
// exchange. What survives is the quantity being measured.
//
// Resolution is one GPTMR tick, 41.7 ns. That is coarse against the ~42 ns the
// timeline is expected to achieve, but it is coarse in a harmless way: the
// quantisation is zero-mean and dithered by the real jitter, so over a thousand
// exchanges the MEAN converges to well under a nanosecond, and the SPREAD only
// grows by the quadrature term (42 -> 44 ns), which can be subtracted out.

#include <cstdint>

namespace librmcs::firmware::sync::pulse {

#if defined(LIBRMCS_APP_PULSE_TEST) && LIBRMCS_APP_PULSE_TEST

inline constexpr bool kEnabled = true;

// 24 MHz / 8 kHz microframes. Exact, because both come from the crystal.
inline constexpr std::uint32_t kTicksPerMicroframe = 3000;

// Full-rate ring relating microframes to GPTMR ticks, 4 ms deep. Full-rate on
// purpose: a decimated history makes the interpolation span the decimation
// period, which is how an earlier revision handed back the precision the
// hardware capture had just won.
inline constexpr std::uint32_t kHistoryDepth = 32;

// Pin mux, GPTMR clock, channel setup. Must run after the UART driver, whose
// pin mux this deliberately overrides.
void init();

// SOF interrupt: record where the GPTMR counter stood at this microframe.
void note_sof(std::uint64_t microframe);

// Arm a hardware pulse for the given absolute microframe. False when the
// timeline cannot place it, or the target is too near (the compare must be
// written before the counter reaches it) or too far to trust.
bool schedule(std::uint64_t microframe);

// Main loop: hands back one captured pulse, converted to the shared axis in
// Q16 microframes. False when nothing new arrived.
bool take_capture(std::uint64_t& microframe_q16);

// Capture interrupt entry point (bound to IRQn_GPTMR0 in the .cpp).
void isr_handler();

// Self-check, published so the assumption "exactly 3000 ticks per microframe"
// is verified on hardware rather than trusted: measured ticks between the two
// oldest and newest history entries, divided by the microframes between them,
// in Q16. Exactly 3000<<16 means the clock tree is what the datasheet says.
std::uint32_t measured_ticks_per_microframe_q16();

#else

inline constexpr bool kEnabled = false;

inline void init() {}
inline void note_sof(std::uint64_t) {}
inline bool schedule(std::uint64_t) { return false; }
inline bool take_capture(std::uint64_t&) { return false; }
inline std::uint32_t measured_ticks_per_microframe_q16() { return 0; }

#endif

} // namespace librmcs::firmware::sync::pulse
