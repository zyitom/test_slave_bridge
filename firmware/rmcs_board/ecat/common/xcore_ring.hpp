#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace librmcs::firmware::ecat {

// Lock-free Single-Producer/Single-Consumer byte ring for CROSS-CORE use.
//
// Unlike the single-core ring buffers under app/src (which pair relaxed
// atomics with atomic_signal_fence -- a compiler-barrier-only recipe that is
// valid against interrupts on one hart but NOT against another hart), this
// ring uses real acquire/release ordering, which the compiler lowers to
// RISC-V fence instructions. Correctness across harts additionally requires
// the backing storage to be non-cacheable: on HPM6E80 this holds for
// SHARE_RAM, which board_init_pmp() maps as MEM_TYPE_MEM_NON_CACHE_BUF with
// AMO enabled on both cores (boards/hpm6e00evk/board.c).
//
// Instances live inside XcoreChannel, which core0 placement-constructs in
// SHARE_RAM before core1 is released (see xcore_channel.hpp); no constructor
// ever runs on core1 and no destructor runs at all.
template <std::size_t buffer_size>
class XcoreRing {
public:
    static_assert(buffer_size >= 2 && (buffer_size & (buffer_size - 1)) == 0);
    static constexpr std::size_t kMask = buffer_size - 1;

    XcoreRing() = default;
    XcoreRing(const XcoreRing&) = delete;
    XcoreRing& operator=(const XcoreRing&) = delete;
    XcoreRing(XcoreRing&&) = delete;
    XcoreRing& operator=(XcoreRing&&) = delete;

    // Owner-side initialization only (core0, before core1 is released).
    void reset() noexcept {
        in_.store(0, std::memory_order::relaxed);
        out_.store(0, std::memory_order::relaxed);
    }

    // Producer side. All-or-nothing: returns false without writing anything
    // when the contiguous free space is smaller than data.size().
    bool try_push(std::span<const std::byte> data) noexcept {
        const std::uint32_t in = in_.load(std::memory_order::relaxed);
        // Acquire pairs with the consumer's release store of out_: the slots
        // being reused below are guaranteed to have been fully read.
        const std::uint32_t out = out_.load(std::memory_order::acquire);

        const auto used = static_cast<std::size_t>(in - out);
        if (buffer_size - used < data.size())
            return false;

        const std::size_t offset = in & kMask;
        const std::size_t slice = std::min(data.size(), buffer_size - offset);
        std::memcpy(buffer_ + offset, data.data(), slice);
        std::memcpy(buffer_, data.data() + slice, data.size() - slice);

        // Release publishes the payload bytes together with the new index.
        in_.store(in + static_cast<std::uint32_t>(data.size()), std::memory_order::release);
        return true;
    }

    // Consumer side: copies out at most destination.size() bytes and returns
    // the number of bytes actually copied.
    std::size_t pop(std::span<std::byte> destination) noexcept {
        // Acquire pairs with the producer's release store of in_.
        const std::uint32_t in = in_.load(std::memory_order::acquire);
        const std::uint32_t out = out_.load(std::memory_order::relaxed);

        const auto readable = static_cast<std::size_t>(in - out);
        const std::size_t count = std::min(readable, destination.size());
        if (count == 0)
            return 0;

        const std::size_t offset = out & kMask;
        const std::size_t slice = std::min(count, buffer_size - offset);
        std::memcpy(destination.data(), buffer_ + offset, slice);
        std::memcpy(destination.data() + slice, buffer_, count - slice);

        // Release hands the consumed slots back to the producer.
        out_.store(out + static_cast<std::uint32_t>(count), std::memory_order::release);
        return count;
    }

    // Diagnostic snapshots; exact only on the respective owning side.
    std::size_t readable() const noexcept {
        return static_cast<std::size_t>(
            in_.load(std::memory_order::acquire) - out_.load(std::memory_order::relaxed));
    }

private:
    // Free-running 32-bit indices; unsigned wrap keeps (in - out) correct.
    std::atomic<std::uint32_t> in_{0};
    std::atomic<std::uint32_t> out_{0};
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::byte buffer_[buffer_size];
};

} // namespace librmcs::firmware::ecat
