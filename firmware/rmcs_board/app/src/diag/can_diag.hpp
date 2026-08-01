#pragma once

// Telemetry for the CAN forwarding stall described in
// ecat/CORE_SWAP_MIGRATION.md section 6: on the single-core image (USB + CAN on
// the same PLIC) the host stops receiving forwarded frames at high load, and the
// condition is permanent until reset. The board has no JTAG or serial console
// wired here, so the state that would normally be read with a halting debugger
// is sampled from the main loop and shipped out as a UART0 uplink frame instead.
//
// The host session provably survives the stall -- protocol::Handler terminates
// the process if a keepalive ack goes unanswered, and the original runs kept
// printing for their full 20 s -- so the uplink is still alive while CAN traffic
// is not. That is what makes main-loop telemetry a usable channel: whatever
// stops, it is not the path this record travels on.
//
// The record deliberately mixes three layers so one snapshot can tell them
// apart:
//   * ISR entry counts        -- is the CAN interrupt still being delivered?
//   * MCAN IR/RXF0S/PSR/ECR   -- does the controller still see bus traffic?
//   * PLIC pending/enable     -- is a source asserting but never delivered?
// A frozen entry count with a non-empty RXF0S and a set PLIC pending bit means
// the interrupt was lost between the gateway and the core; a frozen entry count
// with an idle RXF0S and a bus-off PSR means the controller stopped instead.
//
// Compiled out unless LIBRMCS_APP_CAN_DIAG is set, so the forwarding hot path
// carries nothing in production builds.

#include <cstddef>
#include <cstdint>

namespace librmcs::firmware::diag {

#if defined(LIBRMCS_APP_CAN_DIAG) && LIBRMCS_APP_CAN_DIAG

inline constexpr bool kEnabled = true;

// Wire format of the UART0 uplink payload. Little endian, fixed layout so the
// host decoder needs no length negotiation.
inline constexpr std::uint8_t kRecordMagic = 0xD1U;
inline constexpr std::uint8_t kRecordVersion = 4U;

// Hot-path notifications. All are a single relaxed atomic add; the CAN ISR calls
// the first two per interrupt and per forwarded frame respectively.
void note_isr_entry(std::size_t can_index);
void note_frame(std::size_t can_index);
void note_tx_fail(std::size_t can_index);
void note_alloc_fail();

// One increment per main-loop iteration. Reported as iterations since the
// previous record, which divided by the record period gives the loop period --
// the latency a downlink byte or an uplink batch pays waiting for its turn.
void note_main_loop();

// Counted by Can::poll() each time it releases a stuck PLIC claim.
void note_irq_recovered(std::size_t can_index);

// Main-loop sampler. Emits one record every kEmitPeriodMs of the 1 kHz tick;
// a no-op when the uplink is not carrying data.
void poll(std::uint32_t tick);

#else

inline constexpr bool kEnabled = false;

inline void note_isr_entry(std::size_t) {}
inline void note_frame(std::size_t) {}
inline void note_tx_fail(std::size_t) {}
inline void note_alloc_fail() {}
inline void note_main_loop() {}
inline void note_irq_recovered(std::size_t) {}
inline void poll(std::uint32_t) {}

#endif

} // namespace librmcs::firmware::diag
