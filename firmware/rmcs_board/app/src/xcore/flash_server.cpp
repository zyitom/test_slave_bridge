#include "firmware/rmcs_board/app/src/xcore/flash_server.hpp"

// Single-core builds get an inline no-op from the header, so this translation
// unit is empty for them (the app CMakeLists globs it in either way).
#if defined(LIBRMCS_APP_RELEASE_CORE1) && LIBRMCS_APP_RELEASE_CORE1

# include <cstdint>

# include <board.h>
# include <hpm_clock_drv.h>
# include <hpm_common.h>
# include <hpm_csr_regs.h>
# include <hpm_interrupt.h>
# include <hpm_l1c_drv.h>
# include <hpm_mbx_drv.h>
# include <hpm_romapi.h>
# include <hpm_soc.h>

# include "firmware/rmcs_board/app/src/xcore/secondary_core.hpp"
# include "firmware/rmcs_board/common/foe_staging.hpp"
# include "firmware/rmcs_board/ecat/common/xcore_channel.hpp"

# if !defined(BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR)
#  error "flash_server needs BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR; core1 releases are hpm6e8y only"
# endif

namespace librmcs::firmware::xcore {
namespace {

// The ONLY addresses this server will ever touch. Mirrors
// ecat/core1_ecat/src/ecat_config.h (FLASH_EEPROM_ADDR / FLASH_EEPROM_SIZE);
// core1 cross-checks the pair at startup, and every request is validated
// against it here regardless.
constexpr std::uint32_t kWindowStart =
    static_cast<std::uint32_t>(BOARD_FLASH_BASE_ADDRESS)
    + static_cast<std::uint32_t>(BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR);
constexpr std::uint32_t kWindowSize = 0x10000U;
constexpr std::uint32_t kWindowEnd = kWindowStart + kWindowSize;
constexpr std::uint32_t kSectorSize = 4096U;

// Second accepted range: the FoE staging region (metadata sector + candidate
// image, which are adjacent, so one range covers both). See
// common/foe_staging.hpp.
//
// This is an ADDITIONAL window, not a relaxation. core1 is the side holding
// master-supplied bytes, so it must never be able to name an address outside
// what this core is willing to write -- above all not the bootloader, and not
// the app slot core0 is executing from. Adding FoE means listing one more
// legal destination here, never removing the check.
//
// Unlike the EEPROM window this one is not published through the channel:
// both images derive it from the same board.h constants at compile time, so
// there is nothing for core1 to discover, and one fewer runtime value that
// could disagree between the two halves.
struct FlashWindow {
    std::uint32_t start;
    std::uint32_t end;
};

constexpr FlashWindow kWindows[] = {
    {kWindowStart, kWindowEnd},
#if defined(BOARD_FOE_STAGING_ADDR)
    {static_cast<std::uint32_t>(foe::kStagingMetadataStart),
     static_cast<std::uint32_t>(foe::kStagingImageEnd)},
#endif
};

// Program: the whole [address, address+size) must sit inside ONE window.
// Ordered so the subtraction cannot underflow.
bool program_range_allowed(std::uint32_t address, std::uint32_t size) {
    for (const auto& window : kWindows) {
        if (address >= window.start && address < window.end && size <= window.end - address)
            return true;
    }
    return false;
}

// Erase: the address must be a sector boundary MEASURED FROM ITS OWN WINDOW's
// start, not from the flash base, so a window that is not itself sector-aligned
// can never be erased past its own edge.
bool erase_sector_allowed(std::uint32_t address) {
    for (const auto& window : kWindows) {
        if (address >= window.start && address < window.end
            && ((address - window.start) % kSectorSize) == 0U)
            return true;
    }
    return false;
}

// Strictly below the USB (2) and CAN interrupts: this handler can hold the core
// for a whole sector erase, so it must never be the reason a higher-priority
// source is late to start. Once it is running the ROM API masks everything
// anyway -- the priority only decides who gets in first.
constexpr std::uint32_t kFlashRpcIrqPriority = 1U;

xpi_nor_config_t g_nor_config{};
bool g_available = false;
ecat::XcoreFlashRpc* g_rpc = nullptr;

// Masking is MANDATORY here, not decorative: the SDK is built without
// DISABLE_IRQ_PREEMPTIVE, so ENTER_NESTED_IRQ_HANDLING_M() sets mstatus.MIE on
// entry to every vectored handler. Without the mask a higher-priority ISR would
// preempt the ROM call and fetch from a busy XPI. That -- not RAM residency --
// is the load-bearing part; the ROM API's own code lives in Boot ROM, not in the
// NOR, and restores XIP before returning, which is why the bootloader's XpiNor
// (bootloader/src/flash/xpi_nor.hpp) gets away with a plain flash_xip build.
//
// ATTR_RAMFUNC is hardening on top. noinline is what makes it real: inlined into
// the caller these bodies inherit the caller's section, and the attribute would
// be silently defeated. Verify with `objdump -h`: they must appear in .fast.
ATTR_RAMFUNC __attribute__((noinline)) hpm_stat_t
    program_masked(std::uint32_t offset, const std::uint32_t* source, std::uint32_t size) {
    const std::uint32_t flags = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    const hpm_stat_t status = rom_xpi_nor_program(
        BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &g_nor_config, source, offset, size);
    restore_global_irq(flags & CSR_MSTATUS_MIE_MASK);
    return status;
}

ATTR_RAMFUNC __attribute__((noinline)) hpm_stat_t erase_sector_masked(std::uint32_t offset) {
    const std::uint32_t flags = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    const hpm_stat_t status = rom_xpi_nor_erase_sector(
        BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &g_nor_config, offset);
    restore_global_irq(flags & CSR_MSTATUS_MIE_MASK);
    return status;
}

void invalidate_range(std::uint32_t address, std::uint32_t size) {
    const std::uint32_t start = HPM_L1C_CACHELINE_ALIGN_DOWN(address);
    const std::uint32_t end = HPM_L1C_CACHELINE_ALIGN_UP(address + size);
    l1c_dc_invalidate(start, end - start);
}

ecat::XcoreFlashStatus execute(ecat::XcoreFlashOp op, std::uint32_t address, std::uint32_t size) {
    if (!g_available)
        return ecat::XcoreFlashStatus::kUnavailable;

    switch (op) {
    case ecat::XcoreFlashOp::kProgram: {
        if (size == 0 || size > ecat::kXcoreFlashPayloadSize
            || !program_range_allowed(address, size))
            return ecat::XcoreFlashStatus::kBadRange;
        // The payload buffer is 4-byte aligned inside SHARE_RAM, which is what
        // rom_xpi_nor_program's uint32_t* source wants; e2p's own callers pass
        // 2-byte-aligned pointers, so staging through the channel removes an
        // unaligned access the SDK port layer has always had.
        const auto* source = reinterpret_cast<const std::uint32_t*>(g_rpc->payload);
        const hpm_stat_t status = program_masked(
            address - static_cast<std::uint32_t>(BOARD_FLASH_BASE_ADDRESS), source, size);
        if (status != status_success)
            return ecat::XcoreFlashStatus::kFlashError;
        invalidate_range(address, size);
        return ecat::XcoreFlashStatus::kOk;
    }
    case ecat::XcoreFlashOp::kEraseSector: {
        if (!erase_sector_allowed(address))
            return ecat::XcoreFlashStatus::kBadRange;
        const hpm_stat_t status =
            erase_sector_masked(address - static_cast<std::uint32_t>(BOARD_FLASH_BASE_ADDRESS));
        if (status != status_success)
            return ecat::XcoreFlashStatus::kFlashError;
        invalidate_range(address, kSectorSize);
        return ecat::XcoreFlashStatus::kOk;
    }
    case ecat::XcoreFlashOp::kNone:
    default: return ecat::XcoreFlashStatus::kBadOp;
    }
}

void service_request() {
    if (g_rpc == nullptr)
        return;

    ecat::XcoreFlashRpc& rpc = *g_rpc;
    const std::uint32_t request = rpc.request.load(std::memory_order::acquire);
    // A re-poke from core1's wait loop, or a doorbell for a request already
    // answered. Draining the mailbox was the whole job.
    if (request == rpc.response.load(std::memory_order::relaxed))
        return;

    const auto op = static_cast<ecat::XcoreFlashOp>(rpc.op);
    const std::uint32_t address = rpc.address;
    const std::uint32_t size = rpc.size;

    rpc.status = static_cast<std::uint32_t>(execute(op, address, size));
    rpc.response.store(request, std::memory_order::release);
}

} // namespace

extern "C" {

SDK_DECLARE_EXT_ISR_M(IRQn_MBX1A, librmcs_flash_rpc_isr)
void librmcs_flash_rpc_isr(void) {
    // Drain every pending word before servicing: core1 re-pokes while it waits,
    // and an undrained word keeps the interrupt asserted. The values carry no
    // information -- the request slot in SHARE_RAM is the source of truth.
    std::uint32_t message = 0;
    while (mbx_retrieve_message(HPM_MBX1A, &message) == status_success) {}
    service_request();
}

} // extern "C"

void flash_server_init() {
    ecat::XcoreChannel* const channel_ptr = channel();
    if (channel_ptr == nullptr)
        return; // publish_channel() has not run; nothing to publish into.

    ecat::XcoreFlashRpc& rpc = channel_ptr->flash;

    xpi_nor_config_option_t option{};
    option.header.U = BOARD_APP_XPI_NOR_CFG_OPT_HDR;
    option.option0.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT0;
    option.option1.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT1;

    // Auto-config reprograms the very controller this core fetches from, so it
    // runs masked and exactly once -- and only here, never on core1.
    std::uint32_t sector_size = 0;
    const std::uint32_t flags = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    hpm_stat_t status = rom_xpi_nor_auto_config(BOARD_APP_XPI_NOR_XPI_BASE, &g_nor_config, &option);
    if (status == status_success)
        status = rom_xpi_nor_get_property(
            BOARD_APP_XPI_NOR_XPI_BASE, &g_nor_config, xpi_nor_property_sector_size, &sector_size);
    restore_global_irq(flags & CSR_MSTATUS_MIE_MASK);

    g_available = (status == status_success) && (sector_size == kSectorSize);

    rpc.window_start = kWindowStart;
    rpc.window_end = kWindowEnd;
    rpc.request.store(0, std::memory_order::relaxed);
    rpc.response.store(0, std::memory_order::relaxed);
    // Published last, and zero when the NOR did not come up: core1 treats a zero
    // sector size as "no server" and keeps its EEPROM read-only rather than
    // waiting out the full deadline on every request.
    rpc.sector_size = g_available ? kSectorSize : 0U;

    g_rpc = &rpc;

    // MBX1, not the MBX0 pair the hot-path doorbell uses: mbx_send_message()
    // has a single word register and fails while one is pending, so sharing it
    // would let a flash RPC and an uplink poke drop each other
    // (ecat/CORE_SWAP_MIGRATION.md section 3.1). core1 sends on MBX1B and this
    // core takes IRQn_MBX1A. The clock gate is shared by both halves and must be
    // open before core1 is released.
    clock_add_to_group(clock_mbx1, 0);
    mbx_init(HPM_MBX1A);
    mbx_enable_intr(HPM_MBX1A, MBX_CR_RWMVIE_MASK);
    intc_m_enable_irq_with_priority(IRQn_MBX1A, kFlashRpcIrqPriority);
}

} // namespace librmcs::firmware::xcore

#endif
