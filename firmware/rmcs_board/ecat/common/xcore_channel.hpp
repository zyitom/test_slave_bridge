#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

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
// observe a half-initialized channel.

inline constexpr std::uint32_t kXcoreChannelMagic = 0x524D5843U; // "RMXC"
inline constexpr std::uint32_t kXcoreChannelVersion = 1;
inline constexpr std::size_t kXcoreShareRamSize = 16 * 1024;

// core0 -> core1: host command stream (fed to the protocol deserializer on
// the fieldbus core in later phases).
inline constexpr std::size_t kXcoreDownRingSize = 4096;
// core1 -> core0: telemetry stream towards the host.
inline constexpr std::size_t kXcoreUpRingSize = 8192;

struct XcoreChannel {
    std::atomic<std::uint32_t> magic{0};
    std::uint32_t version = 0;
    // Bumped by core0 on every SAFEOP -> OP transition so the fieldbus core
    // can observe that the host link (re)started. The ring lifecycle across
    // OP cycles (flush vs. keep) is deliberately NOT decided here; it belongs
    // to the protocol session policy (see ../README.md).
    std::atomic<std::uint32_t> link_epoch{0};
    XcoreRing<kXcoreDownRingSize> down;
    XcoreRing<kXcoreUpRingSize> up;
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
