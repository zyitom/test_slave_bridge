#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Sibling include, deliberately not the repo-root form used elsewhere: core1's
// EtherCAT image puts directories on the include path rather than the project
// root (see ecat/core1_ecat/CMakeLists.txt), so a rooted path would not resolve
// there. A quoted sibling resolves for every consumer.
#include "foe_staging.hpp"

// Writer for the FoE staging region, shared by both producers.
//
// There are two, and they reach the flash by different routes:
//
//   * core1's FoE handler, whose bytes arrive from the EtherCAT master and cross
//     to core0 through the MBX1 flash RPC;
//   * core0's USB self-test path, which is already on the core that owns the NOR
//     and calls the masked ROM helpers directly.
//
// They share THIS file rather than each driving the flash themselves, because
// what has to be identical between them is not the transport but the ORDER of
// writes -- that order is the entire crash-safety argument in foe_staging.hpp,
// and two copies of it would be two chances to get it subtly wrong. The flash
// primitives are therefore injected, and the sequencing lives here once.

namespace librmcs::firmware::foe {

#if defined(BOARD_FOE_STAGING_ADDR)

// Erase one 4 KiB sector / program `size` bytes. Both return false on failure
// and must be synchronous: this class treats a false as final.
struct StagingFlashOps {
    bool (*erase_sector)(std::uint32_t address, void* context);
    bool (*program)(std::uint32_t address, const void* data, std::uint32_t size, void* context);
    // Largest `size` a single program call accepts. The RPC path caps this at
    // kXcoreFlashPayloadSize (512); the local path has no such limit, but takes
    // the same chunking so both are exercised by the same tests.
    std::uint32_t max_program_size;
    void* context;
};

inline constexpr std::uint32_t kStagingSectorSize = 4096U;

class StagingWriter {
public:
    // Opens a session: erases the metadata sector and programs the magic, which
    // is what marks the region as "being written" for anyone who reads it before
    // commit(). Erasing metadata FIRST means an interruption from here on can
    // never leave a stale Ready record pointing at a half-overwritten image.
    bool begin(const StagingFlashOps& ops) {
        ops_ = ops;
        next_offset_ = 0U;
        erased_through_ = 0U;
        open_ = false;

        if (ops_.erase_sector == nullptr || ops_.program == nullptr
            || ops_.max_program_size == 0U)
            return false;

        if (!ops_.erase_sector(static_cast<std::uint32_t>(kStagingMetadataStart), ops_.context))
            return false;

        const std::uint32_t magic = kStagingMagic;
        if (!ops_.program(
                static_cast<std::uint32_t>(kStagingMetadataStart), &magic, sizeof(magic),
                ops_.context))
            return false;

        open_ = true;
        return true;
    }

    // Outcome of one erase step. Callers drive erase_step() until kReady before
    // calling write(); each call performs AT MOST ONE sector erase.
    enum class EraseStep : std::uint8_t {
        kReady,      // [0, end) is erased; write() may proceed
        kProgressed, // one sector erased, call again
        kFailed,
    };

    // Erase forward so that [0, end) of the image region is erased, ONE SECTOR
    // PER CALL.
    //
    // The one-per-call bound is why this is not simply inlined into write().
    // A sector erase costs core0 tens of milliseconds with interrupts masked,
    // and on this board every erase is a cross-core RPC that the caller
    // busy-waits on. Doing several of them inside one FoE callback pushes the
    // response past the master's mailbox timeout -- measured as a failed
    // download whose bytes had in fact all been stored. Splitting lets the FoE
    // layer answer BUSY between operations, which is exactly what that part of
    // the protocol is for.
    //
    // Sectors are erased as the write front first reaches them rather than all
    // at once: the region is nearly 2 MiB, and erasing it up front would stall
    // for tens of seconds before the first byte could be accepted.
    EraseStep erase_step(std::uint32_t end) {
        if (!open_)
            return EraseStep::kFailed;
        if (erased_through_ >= end)
            return EraseStep::kReady;

        const std::uint32_t sector_base =
            static_cast<std::uint32_t>(kStagingImageStart) + erased_through_;
        if (!ops_.erase_sector(sector_base, ops_.context))
            return EraseStep::kFailed;
        erased_through_ += kStagingSectorSize;

        // ALWAYS kProgressed after doing an erase, even when the range is now
        // fully erased. Returning kReady here would let the caller program in
        // the same invocation, which is the very pairing this split exists to
        // prevent -- and with 128-byte FoE blocks a sector erase covers 32 of
        // them, so that pairing would be the common case, not a corner one.
        return EraseStep::kProgressed;
    }

    // Append `size` bytes at `offset`. FoE delivers strictly sequential offsets,
    // and erase_step()'s forward-only front depends on that: a sector is erased
    // once, as the front first reaches it, and never revisited. A non-sequential
    // offset is therefore rejected rather than silently programming into a sector
    // that was already written -- on NOR that ANDs the bits together and corrupts
    // quietly.
    //
    // Note this also makes the BUSY retry safe: the master re-sends the SAME
    // block after a BUSY, so the offset it repeats still equals next_offset_ and
    // the retry is accepted exactly once.
    bool write(std::uint32_t offset, const void* data, std::uint32_t size) {
        if (!open_ || offset != next_offset_)
            return false;
        if (size == 0U)
            return true;
        if (offset > kStagingMaxImageSize || size > kStagingMaxImageSize - offset)
            return false;

        // The destination must already be erased. write() deliberately does not
        // erase on demand -- see erase_step() for why that split exists. A caller
        // that skipped it would program un-erased NOR, which ANDs the bits
        // together and corrupts silently, so this is a hard refusal.
        if (offset + size > erased_through_)
            return false;

        const auto* cursor = static_cast<const std::uint8_t*>(data);
        std::uint32_t remaining = size;
        std::uint32_t position = offset;

        while (remaining > 0U) {
            const std::uint32_t chunk =
                remaining < ops_.max_program_size ? remaining : ops_.max_program_size;
            if (!ops_.program(
                    static_cast<std::uint32_t>(kStagingImageStart) + position, cursor, chunk,
                    ops_.context))
                return false;

            cursor += chunk;
            position += chunk;
            remaining -= chunk;
        }

        next_offset_ = position;
        return true;
    }

    // Commits: size first, state last. The state word is the barrier -- until it
    // lands, every reader treats the region as absent. Do not reorder.
    bool commit() {
        if (!open_ || next_offset_ == 0U)
            return false;

        // Addresses computed arithmetically rather than by taking the address of
        // a member through staging_record(): forming a pointer into a flash
        // region we never dereference is pointless, and it is what would make
        // this class untestable off-target.
        constexpr auto kSizeAddress =
            static_cast<std::uint32_t>(kStagingMetadataStart + offsetof(StagingRecord, image_size));
        constexpr auto kStateAddress =
            static_cast<std::uint32_t>(kStagingMetadataStart + offsetof(StagingRecord, state));

        const std::uint32_t size = next_offset_;
        if (!ops_.program(kSizeAddress, &size, sizeof(size), ops_.context))
            return false;

        const std::uint32_t state = kStagingStateReady;
        if (!ops_.program(kStateAddress, &state, sizeof(state), ops_.context))
            return false;

        open_ = false;
        return true;
    }

    // Abandon without committing. Nothing to undo: the metadata sector was
    // erased by begin() and no state word was ever written, so the region
    // already reads as absent. Just stop accepting writes.
    void abort() { open_ = false; }

    bool is_open() const { return open_; }
    std::uint32_t staged_size() const { return next_offset_; }

private:
    StagingFlashOps ops_{};
    std::uint32_t next_offset_ = 0U;
    std::uint32_t erased_through_ = 0U;
    bool open_ = false;
};

#endif

} // namespace librmcs::firmware::foe
