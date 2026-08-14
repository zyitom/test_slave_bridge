/*
 * FoE: receive a firmware image over EtherCAT into the staging region, so the
 * bootloader can install it after the next cold reset.
 *
 * WHY THIS DOES NOT USE THE SDK's hpm_ecat_foe.c.
 *
 * That file implements the same four SSC callbacks, and this started out
 * delegating to it. It cannot be used here for two independent reasons:
 *
 *  1. It demands password 0x87654321 and rejects everything else with
 *     ECAT_FOE_ERRCODE_NORIGHTS. IgH's `ethercat foe_write` has no password
 *     option at all -- its -p is --position -- so it always sends 0, and every
 *     download from an IgH master would fail. The SDK glue is written for
 *     TwinCAT, whose download dialog does have a password field.
 *  2. It programs the NOR inline. This core is a pure RAM image with no access
 *     to the flash at all; every erase and program has to cross to core0 over
 *     the MBX1 RPC.
 *
 * Reimplementing is ~120 lines and removes an SDK dependency, so the four
 * board-side function pointers it exposed are gone with it.
 *
 * THE PASSWORD IS NOT A SECURITY BOUNDARY. It travels in clear on the wire and
 * anything on the segment can read it. It is accepted here only as a typo guard.
 * The actual integrity boundary is the bootloader's SHA-256 check, which runs
 * over the staged image before the app slot is touched at all.
 */

#include <cstddef>
#include <cstdint>

#include "ecat_foe_support.h"

#include "applInterface.h"
#include "boot_mailbox.hpp"
#include "ecat_diag.h"
#include <hpm_l1c_drv.h>

#include "ecat_flash.h"
#include "foe_staging.hpp"
#include "foe_staging_writer.hpp"
#include "xcore_channel.hpp" // kXcoreFlashPayloadSize: the RPC's per-round-trip cap

// Plain include, NOT wrapped in extern "C": the SSC headers carry their own
// linkage guards, and wrapping them again re-declares FOE_Read and friends with
// C linkage against the declarations applInterface.h already pulled in.
#include "ecatfoe.h" // FOE_MAXBUSY_ZERO / FOE_MAXBUSY: the busy return-value band
#include "foeappl.h" // ECAT_FOE_ERRCODE_*

namespace foe = librmcs::firmware::foe;

/* The port-layer macros and foe_staging.hpp's C++ constants describe the same
 * region by two routes. Prove they agree rather than assuming it. */
static_assert(FOE_FILE_ADDR == foe::kStagingImageStart);
static_assert(FOE_FILE_MAX_SIZE == foe::kAppSlotCapacity);
static_assert(FOE_FILE_MAX_SIZE >= foe::kStagingMaxImageSize);

namespace {

/* The only name this slave will accept a firmware image under. A download
 * addressed to anything else is refused rather than guessed at: an operator who
 * mistypes gets an error, not a bricked board. */
constexpr char kImageName[] = "app";
constexpr std::uint16_t kImageNameSize = sizeof(kImageName) - 1U;

/* 0 is what IgH sends (it cannot send anything else); 0x87654321 is the value
 * the SDK sample and its TwinCAT instructions use. Both accepted so either
 * master works -- see the note at the top about what this check is and is not. */
constexpr std::uint32_t kPasswordNone = 0x00000000U;
constexpr std::uint32_t kPasswordLegacy = 0x87654321U;

foe::StagingWriter g_writer;
bool g_reset_requested = false;

/* Set when a download commits; read on the BOOT exit to decide whether that
 * transition should turn into a reboot. Deliberately separate from
 * g_reset_requested: committing and rebooting are two different moments, and
 * collapsing them is what broke the first hardware run. */
bool g_download_complete = false;

/* Set by the last data block, consumed by ecat_foe_poll_reset() on the next main
 * loop pass. Keeps the two commit programs off the acknowledgement path. */
bool g_commit_pending = false;

/* Upload cursor, valid between foe_read() and the last foe_read_data(). */
std::uint32_t g_read_size = 0U;

bool name_matches(std::uint16_t MBXMEM* name, std::uint16_t name_size) {
    if (name_size != kImageNameSize)
        return false;
    const auto* chars = reinterpret_cast<const char*>(name);
    for (std::uint16_t i = 0; i < name_size; ++i) {
        if (chars[i] != kImageName[i])
            return false;
    }
    return true;
}

bool password_ok(std::uint32_t password) {
    return password == kPasswordNone || password == kPasswordLegacy;
}

/* Erase one sector, opening the RPC's erase gate for exactly that call.
 *
 * The gate (ecat_flash.h) keeps the e2p garbage collector from erasing once the
 * data plane is up, because an erase costs core0 tens of milliseconds of masked
 * interrupts. FoE erases are legitimate but happen at runtime, so they would
 * otherwise be refused.
 *
 * Scoped per-call rather than per-session on purpose: a transfer the master
 * abandons half way -- link pulled, master restarted -- must not leave the gate
 * open behind it, and there is no callback that reliably fires for an abandoned
 * transfer. */
/* Instrumentation. Every claim made so far about "the erase is too slow" has
 * been inference; these two counters are the first actual measurement of it. */
std::uint32_t g_erase_count = 0U;
std::uint32_t g_erase_ms_total = 0U;
std::uint32_t g_block_count = 0U;

bool rpc_erase_sector(std::uint32_t address, void* /*context*/) {
    const std::uint32_t t0 = HW_GetTimer();
    ecat_flash_open_boot_window();
    const hpm_stat_t status = ecat_flash_rpc_erase(address, foe::kStagingSectorSize);
    ecat_flash_close_boot_window();
    const std::uint32_t elapsed = HW_GetTimer() - t0;

    ++g_erase_count;
    g_erase_ms_total += elapsed;
    ecat_diag_printf(
        "foe: erase#%u @0x%x %u ms (blk %u)\n", (unsigned)g_erase_count, (unsigned)address,
        (unsigned)elapsed, (unsigned)g_block_count);

    return status == status_success;
}

bool rpc_program(std::uint32_t address, const void* data, std::uint32_t size, void* /*context*/) {
    return ecat_flash_rpc_program(static_cast<const std::uint8_t*>(data), address, size)
        == status_success;
}

/* Read back through the XIP window.
 *
 * The invalidate is not optional and not symmetry with core0: core0 programmed
 * these bytes over the RPC and invalidated ITS OWN cache afterwards
 * (flash_server.cpp), which does nothing for this core's D-cache. Without this,
 * an upload issued right after a download can serve pre-erase content and
 * "verify" successfully against nothing.
 */
void read_staged(std::uintptr_t address, void* destination, std::uint32_t size) {
    const std::uint32_t start = HPM_L1C_CACHELINE_ALIGN_DOWN(address);
    const std::uint32_t end = HPM_L1C_CACHELINE_ALIGN_UP(address + size);
    l1c_dc_invalidate(start, end - start);
    __builtin_memcpy(destination, reinterpret_cast<const void*>(address), size);
}

/* max_program_size is the RPC payload, not a NOR page: one round trip carries at
 * most this many bytes. A FoE block is bounded by the mailbox the master
 * configured (SII default 0x80, capped by MAX_MBX_SIZE 0x10C) minus the mailbox
 * and FoE headers, so in practice a block always crosses in one round trip and
 * this chunking never engages. It is here so a larger mailbox cannot silently
 * truncate a write. */
constexpr foe::StagingFlashOps kFlashOps{
    .erase_sector = rpc_erase_sector,
    .program = rpc_program,
    .max_program_size = librmcs::firmware::ecat::kXcoreFlashPayloadSize,
    .context = nullptr,
};

} // namespace

extern "C" {

/* --- download (master -> slave) ------------------------------------------ */

UINT16 rmcs_foe_write(UINT16 MBXMEM* name, UINT16 name_size, UINT32 password) {
    /* BOOT only. Not a formality: a flash erase costs core0 tens of milliseconds
     * with interrupts masked, which in OP would overrun the MCAN receivers and
     * NAK an enumerated USB host off its session. BOOT is the state EtherCAT
     * defines for exactly this, and in it there is no process data to disturb. */
    if ((nAlStatus & STATE_MASK) != STATE_BOOT) {
        ecat_diag_printf("foe: write refused, not in BOOT (al=0x%x)\n", nAlStatus);
        return ECAT_FOE_ERRCODE_BOOTSTRAPONLY;
    }
    {
        const auto* chars = reinterpret_cast<const char*>(name);
        ecat_diag_printf(
            "foe: write req name_size=%u pw=0x%x [%c%c%c%c]\n", (unsigned)name_size,
            (unsigned)password, name_size > 0 ? chars[0] : '.', name_size > 1 ? chars[1] : '.',
            name_size > 2 ? chars[2] : '.', name_size > 3 ? chars[3] : '.');
    }

    if (!name_matches(name, name_size)) {
        ecat_diag_printf("foe: name rejected\n");
        return ECAT_FOE_ERRCODE_NOTFOUND;
    }
    if (!password_ok(password)) {
        ecat_diag_printf("foe: password rejected\n");
        return ECAT_FOE_ERRCODE_NORIGHTS;
    }

    /* begin() erases the metadata sector FIRST, so a previously committed image
     * stops being installable the moment a new download starts. Image sectors
     * are erased lazily as the write front reaches them -- erasing the whole
     * region up front would stall core0 for tens of seconds before the master's
     * first data block. */
    if (!g_writer.begin(kFlashOps)) {
        ecat_diag_printf("foe: staging begin failed\n");
        return ECAT_FOE_ERRCODE_FLASH_ERROR;
    }

    g_erase_count = 0U;
    g_erase_ms_total = 0U;
    g_block_count = 0U;
    ecat_diag_printf("foe: download opened\n");
    return 0U;
}

UINT16 rmcs_foe_write_data(UINT16 MBXMEM* data, UINT16 size, UINT8 data_following) {
    if (!g_writer.is_open())
        return ECAT_FOE_ERRCODE_ILLEGAL;

    ++g_block_count;
    /* Sparse on purpose: ~156 blocks for a 20 kB image would flood the ring and
     * the producer drops on overflow, which would hide exactly the tail we are
     * trying to see. First few, then every 32nd. */
    if (g_block_count <= 3U || (g_block_count % 32U) == 0U)
        ecat_diag_printf(
            "foe: blk %u off %u size %u fol %u\n", (unsigned)g_block_count,
            (unsigned)g_writer.staged_size(), (unsigned)size, (unsigned)data_following);

    if (size > 0U) {
        const std::uint32_t offset = g_writer.staged_size();
        if (offset + size > foe::kStagingMaxImageSize) {
            g_writer.abort();
            ecat_diag_printf("foe: image exceeds %u bytes\n", (unsigned)foe::kStagingMaxImageSize);
            return ECAT_FOE_ERRCODE_DISKFULL;
        }

        /* Erase up to the write front, here in the callback.
         *
         * ECAT_FOE_OPCODE_BUSY looks like the right tool -- returning a value in
         * [FOE_MAXBUSY_ZERO, FOE_MAXBUSY] makes ecatfoe.c answer BUSY and the
         * master re-send the same block, which is precisely what the protocol
         * offers for a slave busy programming flash. It was implemented and
         * MEASURED, and it made things worse:
         *
         *   without BUSY: the board logged "staged 20000 bytes" -- the whole
         *                 download arrived -- and only the final ack was late
         *   with BUSY:    the transfer aborted right after "download opened",
         *                 master reporting FOE_PROT_ERROR
         *
         * So IgH's download state machine does not handle the BUSY datagram.
         * The intermediate blocks were never the problem: erase + program in one
         * callback is comfortably inside the master's tolerance. Only the LAST
         * block was, because it also carried the commit -- which is why the
         * commit now happens outside this callback instead (see below).
         *
         * erase_step() keeps its one-sector-per-call shape because that is what
         * makes it testable and bounds a single RPC round trip; the loop here is
         * this caller's choice, not the class's. */
        for (;;) {
            const auto step = g_writer.erase_step(offset + size);
            if (step == foe::StagingWriter::EraseStep::kReady)
                break;
            if (step == foe::StagingWriter::EraseStep::kFailed) {
                g_writer.abort();
                ecat_diag_printf("foe: erase failed at %u\n", (unsigned)offset);
                return ECAT_FOE_ERRCODE_FLASH_ERROR;
            }
        }

        if (!g_writer.write(offset, data, size)) {
            g_writer.abort();
            ecat_diag_printf("foe: staging write failed at %u\n", (unsigned)offset);
            return ECAT_FOE_ERRCODE_FLASH_ERROR;
        }
    }

    if (data_following)
        return 0U;

    /* Last block: DEFER the commit to the main loop and acknowledge now.
     *
     * commit() is two more flash programs behind the one this call already did.
     * Chained here they delay the final acknowledgement past the master's
     * tolerance -- measured directly: the board logged the full 20000 bytes
     * staged, and the master still reported FOE_ACK_ERROR. Every intermediate
     * block, which does the same erase and program without the commit,
     * acknowledged fine. The commit is the only thing that has to move.
     *
     * Deferring is safe because the record's whole design is that the state word
     * is what makes it installable: until the deferred commit lands, the region
     * reads as absent. Losing power between this ack and the commit therefore
     * costs the download, never the installed firmware -- it fails closed. */
    g_commit_pending = true;

    /* Record completion; do NOT reset here.
     *
     * This function has not returned yet, so the FoE acknowledgement for the
     * final block has not been sent. Resetting now destroys it, and the master
     * reports the whole transfer as failed even though every byte was staged --
     * measured exactly that way: FOE_RECEIVE_ERROR, USB re-enumeration and a
     * dropped EtherCAT link, 65 ms after the request. The reset waits for the
     * master to leave BOOT (APPL_StopMailboxHandler), which is the SDK sample's
     * sequencing and the only point where the transfer is provably finished on
     * the wire. */
    ecat_diag_printf(
        "foe: LAST blk %u, %u bytes, %u erases %u ms total\n", (unsigned)g_block_count,
        (unsigned)g_writer.staged_size(), (unsigned)g_erase_count, (unsigned)g_erase_ms_total);
    return 0U;
}

/* --- upload (slave -> master), for verifying what was staged -------------- */

UINT16 rmcs_foe_read(
    UINT16 MBXMEM* name, UINT16 name_size, UINT32 password, UINT16 max_block_size, UINT16* data) {
    {
        const auto* chars = reinterpret_cast<const char*>(name);
        ecat_diag_printf(
            "foe: read req name_size=%u pw=0x%x blk=%u [%c%c%c%c]\n", (unsigned)name_size,
            (unsigned)password, (unsigned)max_block_size, name_size > 0 ? chars[0] : '.',
            name_size > 1 ? chars[1] : '.', name_size > 2 ? chars[2] : '.',
            name_size > 3 ? chars[3] : '.');
    }

    if (!name_matches(name, name_size)) {
        ecat_diag_printf("foe: read name rejected\n");
        return ECAT_FOE_ERRCODE_NOTFOUND;
    }
    if (!password_ok(password)) {
        ecat_diag_printf("foe: read password rejected\n");
        return ECAT_FOE_ERRCODE_NORIGHTS;
    }

    /* Only a COMMITTED image is readable. An in-progress or abandoned session
     * must read as absent rather than as a short file, or a master doing
     * download-then-upload verification would compare against garbage and
     * report success.
     *
     * The record is read through read_staged(), NOT through
     * foe::staging_record_is_ready(): that helper dereferences the XIP address
     * directly, and core1's D-cache still holds the pre-erase content of this
     * sector -- core0 did the programming and invalidated only its own cache.
     * Reading it directly makes a freshly committed record look absent, which is
     * exactly how the first hardware upload failed. */
    foe::StagingRecord record{};
    read_staged(foe::kStagingMetadataStart, &record, sizeof(record));

    if (record.magic != foe::kStagingMagic || record.state != foe::kStagingStateReady
        || record.reserved != foe::kFlashWordErased || record.image_size == 0U
        || record.image_size > foe::kStagingMaxImageSize) {
        ecat_diag_printf(
            "foe: no staged image (magic=0x%x state=0x%x size=%u)\n", (unsigned)record.magic,
            (unsigned)record.state, (unsigned)record.image_size);
        return ECAT_FOE_ERRCODE_NOTFOUND;
    }

    g_read_size = record.image_size;

    const std::uint32_t count = g_read_size < max_block_size ? g_read_size : max_block_size;
    read_staged(foe::kStagingImageStart, data, count);
    return static_cast<UINT16>(count);
}

UINT16 rmcs_foe_read_data(UINT32 offset, UINT16 max_block_size, UINT16* data) {
    if (offset >= g_read_size)
        return 0U;

    std::uint32_t count = g_read_size - offset;
    if (count > max_block_size)
        count = max_block_size;

    read_staged(foe::kStagingImageStart + offset, data, count);
    return static_cast<UINT16>(count);
}

/* --- reset ---------------------------------------------------------------- */

bool ecat_foe_download_complete(void) { return g_download_complete; }

void ecat_foe_request_reset(void) { g_reset_requested = true; }

bool ecat_foe_reset_pending(void) { return g_reset_requested; }

/* Performed from the main loop, never from the mailbox callback that set the
 * flag. The reason shows up on the wire: a reset inside the callback discards
 * the FoE response that has not been sent yet, and the master reports a timeout
 * on a transfer that in fact succeeded. Hence the flag, and the wait until the
 * master has left BOOT.
 *
 * The reset is COLD, which is load-bearing: a hot reset restarts CPU0 only,
 * leaving this core running the old image against a freshly booted core0
 * (common/boot_mailbox.hpp). It is issued from here because the mailbox and PPOR
 * are ordinary peripherals either core can reach, and by this point the flash
 * RPC has been idle since the commit.
 *
 * reboot_to_app_once(), not reboot_to_bootloader(): the bootloader should
 * install the staged image and then ENTER the app. Requesting DFU would make it
 * install and then sit waiting for a host that is not coming. */
void ecat_foe_poll_reset(void) {
    /* Runs before the reset check: the commit deferred out of the final data
     * block has to land before anything can act on the record. */
    if (g_commit_pending) {
        g_commit_pending = false;
        if (g_writer.commit()) {
            g_download_complete = true;
            ecat_diag_printf("foe: staged %u bytes\n", (unsigned)g_writer.staged_size());
        } else {
            g_writer.abort();
            ecat_diag_printf("foe: staging commit failed\n");
        }
    }

    if (!g_reset_requested)
        return;
    ecat_diag_printf("foe: resetting to install\n");
    librmcs::firmware::boot::BootMailbox::reboot_to_app_once();
}

bool ecat_foe_support_init(void) {
    /* Hooked unconditionally: the flash RPC has no liveness query, and a check
     * at init would report a liveness that can lapse afterwards anyway. A dead
     * RPC instead surfaces per-request as ECAT_FOE_ERRCODE_FLASH_ERROR, which
     * fails the transfer rather than completing one that went nowhere. */
    pAPPL_FoeRead = rmcs_foe_read;
    pAPPL_FoeReadData = rmcs_foe_read_data;
    pAPPL_FoeWrite = rmcs_foe_write;
    pAPPL_FoeWriteData = rmcs_foe_write_data;

    ecat_diag_printf(
        "foe: ready, name '%s', staging 0x%x max %u\n", kImageName,
        (unsigned)foe::kStagingImageStart, (unsigned)foe::kStagingMaxImageSize);
    return true;
}

} // extern "C"
