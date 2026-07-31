/*
 * core1 flash-RPC client (../../CORE_SWAP_MIGRATION.md section 3.2, migration
 * step 3). Rationale and policy live in ecat_flash.h; this file is the
 * mechanism.
 *
 * Protocol, one outstanding request at a time (core1 is the only client and it
 * busy-waits, so no queue is needed):
 *
 *   core1: fill op/address/size/payload
 *          request.store(seq, release)          <- publishes the arguments
 *          fence; mbx_send_message(HPM_MBX1B)   <- doorbell
 *          spin until response.load(acquire) == seq
 *   core0: IRQn_MBX1A ISR -> drain mailbox -> execute -> status = ...
 *          response.store(request, release)
 *
 * The doorbell carries no information (the request slot is the source of truth),
 * so a lost poke costs latency, not correctness: the wait loop re-pokes
 * periodically. That matters because mbx_send_message() has a single-word
 * register and fails outright when one is already pending.
 */

#include "ecat_flash.h"

#include <cstdint>
#include <cstring>

#include <hpm_clock_drv.h>
#include <hpm_common.h>
#include <hpm_csr_drv.h>
#include <hpm_mbx_drv.h>
#include <hpm_soc.h>

#include "xcore_channel.hpp"

#include "ecat_xcore.h"

// Included last: it defines printf as a macro, which would otherwise collide
// with the standard headers above.
#include "ecat_diag.h"

namespace {

using librmcs::firmware::ecat::kXcoreFlashPayloadSize;
using librmcs::firmware::ecat::XcoreChannel;
using librmcs::firmware::ecat::XcoreFlashOp;
using librmcs::firmware::ecat::XcoreFlashRpc;
using librmcs::firmware::ecat::XcoreFlashStatus;

XcoreFlashRpc* g_rpc = nullptr;
std::uint32_t g_sector_size = 0;
std::uint32_t g_seq = 0;
bool g_boot_window = false;

// Wall-clock bounds for the busy-wait, in core1 CPU cycles. A single sector
// erase is the long pole (tens of ms typical, but NOR datasheets allow several
// hundred), and core0 may be servicing a higher-priority interrupt before it
// gets to the RPC, so the give-up deadline is deliberately generous: a spurious
// timeout would corrupt the e2p state machine's view of what reached flash,
// which is far worse than a long stall during a boot-time SII refresh.
std::uint64_t g_deadline_cycles = 0;
std::uint64_t g_nag_cycles = 0;

constexpr std::uint32_t kDeadlineMs = 5000;
constexpr std::uint32_t kNagIntervalMs = 20;
// Only used if the SYSCTL query fails; the real part runs well above this, so
// the fallback errs towards a LONGER wait in wall-clock terms.
constexpr std::uint32_t kFallbackCpuHz = 200000000;

hpm_stat_t rpc_call(
    XcoreFlashOp op, std::uint32_t address, const std::uint8_t* data, std::uint32_t size) {
    if (g_rpc == nullptr)
        return status_fail;

    g_rpc->op = static_cast<std::uint32_t>(op);
    g_rpc->address = address;
    g_rpc->size = size;
    if (data != nullptr && size != 0)
        std::memcpy(g_rpc->payload, data, size);
    g_rpc->status = static_cast<std::uint32_t>(XcoreFlashStatus::kOk);

    const std::uint32_t seq = ++g_seq;
    g_rpc->request.store(seq, std::memory_order::release);

    // SHARE_RAM is mapped MEM_TYPE_MEM_NON_CACHE_BUF, so the release store above
    // can still be posted when the following device-register write leaves the
    // core. Same fence the uplink doorbell uses; without it core0 can see the
    // poke and read a stale request slot.
    __asm__ volatile("fence" ::: "memory");
    (void)mbx_send_message(HPM_MBX1B, seq);

    const std::uint64_t start = hpm_csr_get_core_mcycle();
    std::uint64_t last_nag = start;
    while (g_rpc->response.load(std::memory_order::acquire) != seq) {
        const std::uint64_t now = hpm_csr_get_core_mcycle();
        if ((now - start) > g_deadline_cycles) {
            printf("flash rpc timeout: op %u addr 0x%08x; disabling flash writes.\n", g_rpc->op,
                address);
            // Retire the client for the rest of this boot. core0 may still be
            // inside the request we gave up on, and it reads the payload buffer
            // while it programs -- issuing a second request would let a new
            // argument set overwrite one in flight. The e2p operation that
            // triggered this is already failing; degrading to a read-only
            // EEPROM is the only state we can still reason about.
            g_rpc = nullptr;
            return status_timeout;
        }
        if ((now - last_nag) > g_nag_cycles) {
            last_nag = now;
            // A poke that the previous one has not been drained for simply
            // fails; the next pass tries again.
            (void)mbx_send_message(HPM_MBX1B, seq);
        }
    }

    if (g_rpc->status != static_cast<std::uint32_t>(XcoreFlashStatus::kOk)) {
        printf("flash rpc failed: op %u addr 0x%08x status %u\n", g_rpc->op, address,
            g_rpc->status);
        return status_fail;
    }
    return status_success;
}

} // namespace

extern "C" bool ecat_flash_rpc_init(
    std::uint32_t window_start, std::uint32_t window_size, std::uint32_t sector_size) {
    XcoreChannel* channel = ecat_xcore_channel();
    if (channel == nullptr) {
        printf("flash rpc: channel not bound.\n");
        return false;
    }

    XcoreFlashRpc& rpc = channel->flash;
    if (rpc.sector_size == 0) {
        // core0 either failed rom_xpi_nor_auto_config() or never called
        // flash_server_init(). Either way there is nobody to answer, and a
        // request would just burn the full deadline.
        printf("flash rpc: core0 published no flash server.\n");
        return false;
    }
    if (rpc.sector_size != sector_size || rpc.window_start != window_start
        || rpc.window_end != window_start + window_size) {
        // The two images disagree about the emulated-EEPROM geometry, which
        // means one of them was built against a different ecat_config.h /
        // board.h. core0 enforces its own window on every request, so nothing
        // unsafe could happen -- but every write would be rejected, and finding
        // out here produces a diagnosable message instead of a silent
        // read-only EEPROM.
        printf("flash rpc: geometry mismatch (core0 0x%08x-0x%08x/%u, core1 0x%08x-0x%08x/%u).\n",
            rpc.window_start, rpc.window_end, rpc.sector_size, window_start,
            window_start + window_size, sector_size);
        return false;
    }

    // Reset only this core's half of the pair. core0 owns MBX1A: it enables the
    // shared clock gate and unmasks the word-received interrupt there before
    // releasing this core, so the gate is guaranteed open by the time we get
    // here (a write to a gated mailbox is silently lost).
    mbx_init(HPM_MBX1B);

    std::uint32_t cpu_hz = clock_get_frequency(clock_cpu1);
    if (cpu_hz == 0)
        cpu_hz = kFallbackCpuHz;
    g_deadline_cycles = static_cast<std::uint64_t>(cpu_hz) * kDeadlineMs / 1000U;
    g_nag_cycles = static_cast<std::uint64_t>(cpu_hz) * kNagIntervalMs / 1000U;

    g_sector_size = sector_size;
    g_rpc = &rpc;
    return true;
}

extern "C" void ecat_flash_open_boot_window(void) { g_boot_window = true; }

extern "C" void ecat_flash_close_boot_window(void) { g_boot_window = false; }

extern "C" bool ecat_flash_boot_window_open(void) { return g_boot_window; }

extern "C" hpm_stat_t ecat_flash_rpc_program(
    const std::uint8_t* buf, std::uint32_t addr, std::uint32_t size) {
    if (g_rpc == nullptr || buf == nullptr)
        return status_fail;

    // eeprom_emulation.c never asks for more than E2P_FLUSH_BUF_SIZE in one
    // call, which is what the payload buffer is sized for; splitting anyway
    // keeps the contract independent of that constant.
    while (size != 0) {
        const std::uint32_t chunk =
            (size > kXcoreFlashPayloadSize) ? kXcoreFlashPayloadSize : size;
        const hpm_stat_t status = rpc_call(XcoreFlashOp::kProgram, addr, buf, chunk);
        if (status != status_success)
            return status;
        addr += chunk;
        buf += chunk;
        size -= chunk;
    }
    return status_success;
}

extern "C" hpm_stat_t ecat_flash_rpc_erase(std::uint32_t start_addr, std::uint32_t size) {
    if (g_rpc == nullptr)
        return status_fail;

    if (!g_boot_window) {
        // The data plane is running. See ecat_flash.h: this is the one operation
        // whose cost (a full sector, core0 masked for tens of ms) is not
        // survivable for USB and MCAN, so it is refused rather than deferred.
        // Every caller turns the failure into an e2p error, which the port layer
        // reports as ESC_EEPROM_EMULATION_ACK_ERROR.
        printf("flash erase refused: 0x%08x+%u outside the boot window.\n", start_addr, size);
        return status_fail;
    }

    if (g_sector_size == 0 || (start_addr % g_sector_size) != 0 || size == 0
        || (size % g_sector_size) != 0) {
        printf("flash erase rejected: 0x%08x+%u not sector aligned.\n", start_addr, size);
        return status_invalid_argument;
    }

    // One sector per round trip. core0 re-enables interrupts between requests,
    // so an eight-sector e2p_format() costs eight bounded windows instead of one
    // unbounded one.
    for (std::uint32_t offset = 0; offset < size; offset += g_sector_size) {
        const hpm_stat_t status =
            rpc_call(XcoreFlashOp::kEraseSector, start_addr + offset, nullptr, 0);
        if (status != status_success)
            return status;
    }
    return status_success;
}
