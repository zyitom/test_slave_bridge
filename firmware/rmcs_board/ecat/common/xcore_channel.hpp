#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "xcore_diag.hpp"
#include "xcore_ring.hpp"

namespace librmcs::firmware::ecat {

// Shared-memory channel between the two HPM6E80 cores, living in the 16 KiB
// SHARE_RAM region. Both cores' SDK linker scripts export the region through
// __share_mem_start__/__share_mem_end__ at the same physical address, and
// board_init_pmp() (run by board_init() on core0 and board_init_core1() on
// core1) maps it MEM_TYPE_MEM_NON_CACHE_BUF with AMO enabled -- the two
// properties the lock-free rings rely on.
//
// Ownership/handshake: core0 placement-constructs the channel and publishes
// it by release-storing the magic BEFORE releasing core1; core1 spins in
// xcore_channel_wait() until the magic appears (acquire), so it can never
// observe a half-initialized channel. core0 remains the boot core and the
// channel owner after the core swap -- it is the one that embeds and releases
// the core1 image, so publication direction is unchanged even though the
// EtherCAT stack moved to core1.
//
// Flush rule (holds today, but only becomes useful after the swap): EACH CORE
// FLUSHES ONLY THE RING IT CONSUMES. core0 drains `down`, core1 drains `up`.
// Each side advances only its own out_ index, which never races the peer's in_.
// After the swap that is exactly one ring per core, so a handover can leave
// both rings clean instead of relying on a session re-handshake to absorb
// leftover bytes.

inline constexpr std::uint32_t kXcoreChannelMagic = 0x524D5843U; // "RMXC"
// Version 3 = the core-swap layout: EtherCAT (SSC + ESC + ARQ endpoint) lives
// on core1, the protocol stack + CAN/UART + USB live on core0. Field semantics
// changed with it (see owner/owner_ack/ecat_claim below), so the version bump
// is what makes a mismatched core0/core1 image pair detectable at runtime
// instead of silently misbehaving.
// Version 4 adds the flash RPC slot, which shifts every field after `diag`.
// Version 5 adds XcoreFlashRpc::eeprom_ready, which shifts `payload` and
// everything after `flash`.
inline constexpr std::uint32_t kXcoreChannelVersion = 5;
inline constexpr std::size_t kXcoreShareRamSize = 16 * 1024;

// Ring direction is defined RELATIVE TO THE HOST and does not depend on which
// core sits at which end -- that is why the names survive the core swap.
//
// down = host -> device. Producer core1 (ESC output hook, PDI ISR context),
// consumer core0 (feeds the protocol deserializer).
// Sized for the ARQ producer only: at most kPdChunkPayloadSize (44) bytes per
// EtherCAT cycle with a window of 2, so 1 KiB is ~23 cycles of core0 stall
// tolerance (1.4-2.9 ms at 62.5-125 us cycles). It no longer has to absorb USB
// bulk bursts, because USB does not traverse the rings in this layout.
inline constexpr std::size_t kXcoreDownRingSize = 1024;
// up = device -> host. Producer core0 (serializer batches), consumer core1
// (ARQ endpoint feeding the ESC input image).
//
// DO NOT SHRINK. XcoreRing::try_push is all-or-nothing and the producer pushes
// a whole InterruptSafeBuffer batch at a time, so the ring must hold the entire
// batch pool: kBatchCount (8) * kProtocolBufferSize (1023) = 8184 <= 8192. The
// consumer now drains only 44 bytes per EtherCAT cycle, which makes the bound
// tighter than it was, not looser.
inline constexpr std::size_t kXcoreUpRingSize = 8192;

#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD
// Hybrid fixed-PDO variant: a SECOND ring pair carries the raw cyclic CAN
// records (16 bytes each, native_can.hpp) alongside the protocol stream, which
// keeps using down/up above. One complete downlink tick is 28 records (448
// bytes); the rings cover that atomically published batch plus uplink headroom.
inline constexpr std::size_t kXcoreMailboxDownRingSize = 512;
inline constexpr std::size_t kXcoreMailboxUpRingSize = 1024;
#endif

// Data-plane owner. Only ONE host transport drives the protocol stack at a
// time; they are mutually exclusive, never concurrent.
enum class XcoreOwner : std::uint32_t {
    kEcat = 0,
    kUsb = 1,
};

// Cross-core NOR flash RPC (migration step 3).
//
// There is one flash on this part (XPI0) and it holds the emulated SII EEPROM.
// Erase/program stall instruction fetch, and core0 is the XIP core -- so core1,
// which is a pure RAM image and immune to those stalls, cannot do it itself and
// delegates here. Reads are NOT delegated: nor_flash_read is a memcpy from the
// XIP window, not a ROM API call, so core1 does them locally (SII upload is a
// hot path).
//
// Why the whole e2p state machine stays on core1 and only these three primitives
// cross: e2p_info_table is 32 KB of RAM-resident index state. If core0 performed
// the writes, core1's index would go stale and the next e2p_read would follow a
// dead data_addr. There is no workable "core1 reads / core0 writes" split.
inline constexpr std::size_t kXcoreFlashPayloadSize = 512; // == E2P_FLUSH_BUF_SIZE

enum class XcoreFlashOp : std::uint32_t {
    kNone = 0,
    kProgram = 1,     // program `size` bytes of `payload` at `address`
    kEraseSector = 2, // erase exactly ONE sector at `address`; `size` ignored
};

enum class XcoreFlashStatus : std::uint32_t {
    kOk = 0,
    kUnavailable = 1, // core0 never brought the NOR up
    kBadOp = 2,
    kBadRange = 3,    // outside the emulated-EEPROM window, or misaligned
    kFlashError = 4,
};

// One request slot. core1 fills op/address/size/payload, then release-stores an
// incremented `request`; core0 executes and echoes it into `response` with
// `status` valid. Single outstanding request by construction -- core1 busy-waits
// for the echo before issuing the next.
//
// Erase is deliberately ONE sector per request rather than a range: core0 masks
// interrupts around each ROM call, so per-sector requests hand interrupts back
// between sectors instead of holding them off for a whole multi-sector erase.
struct XcoreFlashRpc {
    std::uint32_t sector_size = 0;  // published by core0 pre-release; 0 == no server
    std::uint32_t window_start = 0; // inclusive, absolute XIP address
    std::uint32_t window_end = 0;   // exclusive
    std::atomic<std::uint32_t> request{0};
    std::atomic<std::uint32_t> response{0};
    std::uint32_t op = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t status = 0; // valid when response == request
    // core1 -> core0: set once, after ecat_hardware_init() returns, to say that
    // no further EEPROM flash traffic is coming from the boot path (whether it
    // rewrote the SII or found it already current).
    //
    // core0 releases core1 BEFORE it brings USB and the CAN controllers up, and
    // waits on this. A first-boot SII rewrite masks core0's interrupts for tens
    // of milliseconds per sector, which is enough to make an enumerated USB host
    // NAK out of its session and to overrun a running MCAN receiver -- so the
    // window has to close before there is anything to disturb. This is the
    // "put the first-boot EEPROM window before USB/CAN" requirement of
    // ../CORE_SWAP_MIGRATION.md section 3.2, which step 3 left unimplemented.
    std::atomic<std::uint32_t> eeprom_ready{0};
    alignas(4) unsigned char payload[kXcoreFlashPayloadSize] = {};
};

struct XcoreChannel {
    std::atomic<std::uint32_t> magic{0};
    std::uint32_t version = 0;
    // Generation counter for cross-core records. Bumped on any transport-level
    // restart so in-flight hybrid CAN records from a previous ownership
    // interval can be rejected by tag comparison.
    //
    // It no longer doubles as a "restart your session" signal: the session
    // layer now lives on core0 together with the protocol stack, so core0
    // restarts it directly instead of announcing it across the rings. Keeping
    // the two meanings fused would couple core1's epoch bumps (SAFEOP -> OP) to
    // core0's session lifetime, which is not what either side wants.
    std::atomic<std::uint32_t> link_epoch{0};

    // --- Arbitration (see ../CORE_SWAP_MIGRATION.md section 3.1) ------------
    //
    // core0 is the authority: the USB events that trigger a handover arrive on
    // core0, and core0 is the ring end that actually decides whether bytes
    // enter the deserializer. Putting the decision at the execution point means
    // there is no cross-core window to get wrong.
    //
    // Correctness does NOT depend on core1 observing `owner` promptly. core0
    // switches its own down-ring consumption to discard-mode BEFORE announcing,
    // so any chunk core1 pushes in the meantime cannot interleave with USB
    // bytes in the deserializer. Muxing at the consumer is what makes this
    // safe; gating at the producer would always leave a visibility window.
    std::atomic<std::uint32_t> owner{static_cast<std::uint32_t>(XcoreOwner::kEcat)};
    // core1 echoes the owner value it has actually applied, turning "the remote
    // end has gone quiet" into an observable event rather than a blind wait.
    std::atomic<std::uint32_t> owner_ack{static_cast<std::uint32_t>(XcoreOwner::kEcat)};
    // Monotonic counter incremented by core1 when an EtherCAT master enters OP
    // and wants the link back. This is a REQUEST, not a seizure: core0 decides.
    // (The old behaviour -- master entering OP unconditionally stealing the
    // link -- is what let USB keepalive and IgH fight over ownership; see the
    // arbitration note in ../README.md.)
    std::atomic<std::uint32_t> ecat_claim{0};

    // TRANSITIONAL, pre-core-swap only. The current (core0 = EtherCAT + USB)
    // layout broadcasts USB ownership through this field so hybrid core1 can
    // discard stale CAN records. The core-swap layout expresses the same thing
    // through `owner` above, written by the core that owns the USB events.
    //
    // Kept so the existing bridge firmware keeps building and stays available as
    // a regression baseline during the migration. Delete it -- and this comment
    // -- once ecat/core0 and ecat/core1 are retired in favour of the swapped
    // layout; do NOT add new readers.
    std::atomic<std::uint32_t> usb_active{0};

    XcoreRing<kXcoreDownRingSize> down;
    XcoreRing<kXcoreUpRingSize> up;
    // core1 -> core0 text diagnostics; core1 must not printf (see xcore_diag.hpp).
    XcoreDiagRing diag;
    // core1 -> core0 NOR flash delegation for the emulated SII EEPROM.
    XcoreFlashRpc flash;
#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD
    // core0 -> core1: downlink CAN frames to transmit. Hybrid records carry
    // link_epoch low16 so transport handovers invalidate queued commands.
    XcoreRing<kXcoreMailboxDownRingSize> mailbox_down;
    // core1 -> core0: received CAN frames. The same epoch tag prevents feedback
    // from an old ownership interval entering the next fixed PDO image.
    XcoreRing<kXcoreMailboxUpRingSize> mailbox_up;
#endif
};
static_assert(std::is_trivially_destructible_v<XcoreChannel>);
static_assert(sizeof(XcoreChannel) <= kXcoreShareRamSize);

namespace internal {

extern "C" {
// Linker-provided SHARE_RAM boundary; identical in the core0 and core1
// scripts, so both images resolve to the same physical address.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
extern unsigned char __share_mem_start__[];
}

inline void* xcore_channel_memory() noexcept { return static_cast<void*>(__share_mem_start__); }

} // namespace internal

// Core0 only. Construct and publish the channel; must complete before the
// core1 image is released (multicore_release_cpu).
inline XcoreChannel& xcore_channel_init() noexcept {
    auto* channel = ::new (internal::xcore_channel_memory()) XcoreChannel();
    channel->version = kXcoreChannelVersion;
    channel->magic.store(kXcoreChannelMagic, std::memory_order::release);
    return *channel;
}

// Core1 only. Blocks until core0 has published the channel.
inline XcoreChannel& xcore_channel_wait() noexcept {
    auto* channel = static_cast<XcoreChannel*>(internal::xcore_channel_memory());
    while (channel->magic.load(std::memory_order::acquire) != kXcoreChannelMagic) {}
    return *channel;
}

} // namespace librmcs::firmware::ecat
