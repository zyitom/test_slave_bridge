/*
 * READ-ONLY project copy of
 * bsp/hpm_sdk/samples/ethercat/port/hpm_ecat_e2p_emulation.c.
 *
 * Compiled INSTEAD of the SDK file (../CMakeLists.txt adds this path and not the
 * SDK one; sdk_inc order makes the header resolve to the SDK's unchanged
 * hpm_ecat_e2p_emulation.h, so the port layer's prototypes still match). The SDK
 * tree is third-party and stays untouched.
 *
 * Three deviations from the SDK original, all required by the core swap
 * (../../CORE_SWAP_MIGRATION.md section 3.2):
 *
 * 1. nor_flash_init() is NOT called. It wraps rom_xpi_nor_auto_config(), which
 *    RECONFIGURES THE XPI0 CONTROLLER -- and core0 is the XIP core in the new
 *    layout, executing from that very flash. core1 must never touch it. core0
 *    does the auto-config once, before releasing this core, and publishes the
 *    resulting geometry through the cross-core channel; the fields
 *    nor_flash_init() would have filled are set up by hand below.
 *
 * 2. flash_write / flash_erase are cross-core RPCs to core0 (ecat_flash.h),
 *    which executes the ROM API with its interrupts masked. flash_read stays
 *    LOCAL: it is a memcpy from the XIP window plus a cache invalidate, never
 *    goes through the ROM API, and SII upload is a hot path.
 *
 *    Only these two function pointers cross the core boundary. The e2p state
 *    machine -- including its 32 KiB RAM index table -- stays whole on core1,
 *    because a split would leave core1's index table stale after every core0
 *    write and the next e2p_read() would follow a dead data_addr into a CRC
 *    failure.
 *
 * 3. Erases are confined to a boot window, opened and closed inside
 *    ecat_flash_eeprom_init(). An erase costs core0 a whole sector of masked
 *    interrupts (tens of ms: USB NAKs, MCAN RX overruns), so the e2p garbage
 *    collector must never run once the data plane is up. The gate lives in
 *    ecat_flash_rpc_erase(), which covers every path that could erase --
 *    e2p_config()'s version-mismatch format, e2p_write()'s implicit
 *    E2P_FLUSH_FORCE, e2p_clear() -- rather than each call site.
 *
 * Runtime master EEPROM writes are answered, but only below SAFEOP: see
 * ecat_flash_eeprom_write().
 *
 * All printf calls are the same as the SDK original; ecat_diag.h (force-included
 * by the build) redirects them into the cross-core diagnostic ring.
 */

#include "hpm_ecat_e2p_emulation.h"

#include "ecatslv.h" /* bEcatInputUpdateRunning / bEcatOutputUpdateRunning */
#include "hpm_ecat_hw.h"
#include "hpm_nor_flash.h"

#include "ecat_flash.h"

extern unsigned char aEepromData[]; /* EEPROM data is created in eeprom.h by SSC Tool */

static nor_flash_config_t g_nor_cfg;
static e2p_t g_e2p_ctx;

static hpm_stat_t flash_read(uint8_t* buf, uint32_t addr, uint32_t size) {
    return nor_flash_read(&g_nor_cfg, buf, addr, size);
}

/* Both mutating hooks go to core0. They return status_fail when no flash server
 * answered at init, which degrades the emulated EEPROM to read-only instead of
 * corrupting it -- the same behaviour this file had before the RPC landed. */
static hpm_stat_t flash_write(const uint8_t* buf, uint32_t addr, uint32_t size) {
    return ecat_flash_rpc_program(buf, addr, size);
}

static hpm_stat_t flash_erase(uint32_t start_addr, uint32_t size) {
    return ecat_flash_rpc_erase(start_addr, size);
}

/* check EtherCAT Slave Controller Configuration Area is first 8 words(1 word = 2 bytes) */
static hpm_stat_t ecat_flash_eeprom_check_configuration_area(void) {
    hpm_stat_t stat;
    uint16_t config_data[8];
    uint8_t checksum;
    for (uint8_t i = 0; i < 8; i++) {
        /* read Configuration Area data from e2p */
        stat = e2p_read(i, EEPROM_WRITE_SIZE, (uint8_t*)&config_data[i]);
        if (stat != E2P_STATUS_OK) {
            return status_fail;
        }
    }

    /* calculate checksum value for word0 - word6 */
    checksum = ecat_calculate_eeprom_config_data_checksum((uint8_t*)config_data, 14);

    /* Low byte contains remainder of division of word 0 to word 6 as unsigned number
     * divided by the polynomial x8+x2+x+1(initial value 0xFF). */
    if (checksum != config_data[7]) {
        return status_invalid_argument; /* checksum error */
    }

    return status_success;
}

/* Decide whether the built-in SII must be pushed into the emulated EEPROM.
 * Identical policy to the SDK original: a configuration-area checksum failure,
 * a product-code mismatch, or a stored revision older than the built-in one. */
static bool ecat_flash_eeprom_refresh_required(void) {
    if (ecat_flash_eeprom_check_configuration_area() != status_success) {
        printf("Stored EEPROM configuration area checksum error.\n");
        return true;
    }

#if defined(ECAT_EEPROM_CHECK_PRODUCT_CODE_AND_REVISION)                                           \
    && ECAT_EEPROM_CHECK_PRODUCT_CODE_AND_REVISION
    uint16_t product_code_low, product_code_high, revision_low, revision_high;

    if ((e2p_read(
             ESC_EEPROM_PRODUCT_CODE_WORD_INDEX, EEPROM_WRITE_SIZE, (uint8_t*)&product_code_low)
         != E2P_STATUS_OK)
        || (e2p_read(
                ESC_EEPROM_PRODUCT_CODE_WORD_INDEX + 1, EEPROM_WRITE_SIZE,
                (uint8_t*)&product_code_high)
            != E2P_STATUS_OK)
        || (e2p_read(
                ESC_EEPROM_REVISION_NUM_WORD_INDEX, EEPROM_WRITE_SIZE, (uint8_t*)&revision_low)
            != E2P_STATUS_OK)
        || (e2p_read(
                ESC_EEPROM_REVISION_NUM_WORD_INDEX + 1, EEPROM_WRITE_SIZE,
                (uint8_t*)&revision_high)
            != E2P_STATUS_OK)) {
        printf("Read Product Code / Revision Number in EEPROM failed.\n");
        return true;
    }

    const uint32_t product_code = ((uint32_t)product_code_high << 16) | product_code_low;
    const uint32_t revision = ((uint32_t)revision_high << 16) | revision_low;
    const uint32_t built_in_product_code =
        ((uint32_t*)aEepromData)[ESC_EEPROM_PRODUCT_CODE_WORD_INDEX / 2];
    const uint32_t built_in_revision =
        ((uint32_t*)aEepromData)[ESC_EEPROM_REVISION_NUM_WORD_INDEX / 2];

    printf("Stored SII product 0x%x revision %u (built-in 0x%x / %u).\n", product_code, revision,
        built_in_product_code, built_in_revision);

    if ((product_code != built_in_product_code) || (revision < built_in_revision)) {
        return true;
    }
#endif

    return false;
}

hpm_stat_t ecat_flash_eeprom_init(void) {
    hpm_stat_t stat;
    uint16_t data, dummy_data = 0xFFFF;
    uint32_t level;

    g_nor_cfg.xpi_base = BOARD_APP_XPI_NOR_XPI_BASE;
    g_nor_cfg.base_addr = BOARD_FLASH_BASE_ADDRESS;
    g_nor_cfg.opt_header = BOARD_APP_XPI_NOR_CFG_OPT_HDR;
    g_nor_cfg.opt0 = BOARD_APP_XPI_NOR_CFG_OPT_OPT0;
    g_nor_cfg.opt1 = BOARD_APP_XPI_NOR_CFG_OPT_OPT1;
    /* nor_flash_init() deliberately not called (see the file header). Only
     * sector_size would have come out of it, and nor_flash_read() -- the one
     * remaining user of this struct -- ignores every field; state it from the
     * board's flash geometry so it is not left at zero for a future reader. */
    g_nor_cfg.sector_size = ECAT_EEPROM_FLASH_SECTOR_SIZE;

    const uint32_t window_start = g_nor_cfg.base_addr + ECAT_EEPROM_FLASH_OFFSET;
    const uint32_t window_size = ECAT_EEPROM_FLASH_SECTOR_CNT * ECAT_EEPROM_FLASH_SECTOR_SIZE;

    if (!ecat_flash_rpc_init(window_start, window_size, ECAT_EEPROM_FLASH_SECTOR_SIZE)) {
        /* Not fatal. The read path is local and does not need core0, so a valid
         * stored SII still uploads; only refreshes and master writes fail. */
        printf("Flash RPC unavailable; emulated EEPROM is read-only this boot.\n");
    }

    /* Everything that erases is confined to this window. It closes before this
     * function returns, i.e. before ecat_hardware_init() -> MainInit(), so no
     * master command and no PDO cycle can ever be in flight while it is open. */
    ecat_flash_open_boot_window();

    g_e2p_ctx.config.start_addr = window_start;
    g_e2p_ctx.config.erase_size = ECAT_EEPROM_FLASH_SECTOR_SIZE;
    g_e2p_ctx.config.sector_cnt = ECAT_EEPROM_FLASH_SECTOR_CNT;
    g_e2p_ctx.config.version = 0x4553; /* 'E' 'S' */
    g_e2p_ctx.config.flash_read = flash_read;
    g_e2p_ctx.config.flash_write = flash_write;
    g_e2p_ctx.config.flash_erase = flash_erase;

    /* The interrupt mask is the SDK's: it keeps a PDI/timer ISR from observing a
     * half-built index table. It also covers the RPC busy-waits that e2p_config()
     * can trigger (a version mismatch formats the area), which is harmless here
     * -- nothing on this core needs servicing before MainInit(). */
    level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    stat = e2p_config(&g_e2p_ctx);
    restore_global_irq(level);
    if (E2P_STATUS_OK != stat) {
        printf("e2p init failed (%d).\n", stat);
        ecat_flash_close_boot_window();
        return status_fail;
    }

    /* If the value of aEepromData is the initial value in weak declaration, it means there is
     * no valid data in aEepromData */
    if ((aEepromData[0] == 0xa5) && (aEepromData[1] == 0xa5) && (aEepromData[2] == 0xa5)
        && (aEepromData[3] == 0xa5)) {
        printf("No EEPROM content in PROGRAM.\n");
    } else if (ecat_flash_eeprom_refresh_required()) {
        printf("Init EEPROM content.\n");
        level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
        for (uint32_t i = 0; i < ESC_EEPROM_SIZE / EEPROM_WRITE_SIZE; i++) {
            stat = e2p_write(i, EEPROM_WRITE_SIZE, &aEepromData[2 * i]);
            if (stat != E2P_STATUS_OK) {
                break;
            }
        }
        restore_global_irq(level);
        if (stat != E2P_STATUS_OK) {
            printf("Init EEPROM content failed (%d).\n", stat);
            ecat_flash_close_boot_window();
            return status_fail;
        }
        printf("Init EEPROM content successful.\n");
    } else {
        printf("No need to init EEPROM content.\n");
    }

    /* Back-fill words that do not read back, so a partially written area still
     * uploads a full image instead of failing mid-upload with an ack error. */
    level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    for (uint32_t i = 0; i < ESC_EEPROM_SIZE / EEPROM_WRITE_SIZE; i++) {
        stat = e2p_read(i, EEPROM_WRITE_SIZE, (uint8_t*)&data);
        if (stat != E2P_STATUS_OK) {
            stat = e2p_write(i, EEPROM_WRITE_SIZE, (uint8_t*)&dummy_data);
            if (stat != E2P_STATUS_OK) {
                break;
            }
        }
    }
    restore_global_irq(level);

    ecat_flash_close_boot_window();

    if (E2P_STATUS_OK != stat) {
        printf("EEPROM padding sweep failed (%d).\n", stat);
        return status_fail;
    }

    return status_success;
}

hpm_stat_t ecat_flash_eeprom_write(uint32_t addr, uint8_t* data) {
    hpm_stat_t stat;
    uint32_t level;

    /* Runtime policy (../../CORE_SWAP_MIGRATION.md section 3.2 offers "defer to a
     * safe window" or "answer ESC_EEPROM_EMULATION_ACK_ERROR"; this is the
     * second, narrowed to the states where the write actually costs something).
     *
     * A word write is two flash programs, and both e2p and the caller below hold
     * THIS core's interrupts masked across the cross-core round trip -- so in
     * SAFEOP/OP it directly steals PDI ISR time and drops EtherCAT cycles. Below
     * SAFEOP there is no process data to lose, the write is bounded at a few
     * hundred microseconds on core0, and refusing would make the SII
     * unwritable over EtherCAT for no benefit.
     *
     * Note the caller (ecat_eeprom_emulation_write) already skips words whose
     * stored value matches, so a master rewriting an identical SII costs
     * nothing and never reaches here.
     *
     * An erase is a different matter and is refused unconditionally at this
     * point by ecat_flash_rpc_erase(): if this append overflows the active area,
     * e2p's implicit E2P_FLUSH_FORCE fails and the master gets an ack error
     * rather than tens of milliseconds of masked interrupts on core0. */
    if (bEcatInputUpdateRunning || bEcatOutputUpdateRunning) {
        printf("Master EEPROM write to word %u refused: data plane running.\n", addr);
        return status_fail;
    }

    level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    stat = e2p_write(addr, EEPROM_WRITE_SIZE, data);
    restore_global_irq(level);
    if (stat == status_success) {
        e2p_info("\n WRITE success: addr[%d] - val[0x%x]\n", addr, *((uint16_t*)data));
    } else {
        e2p_info("\n WRITE failed: error code[%d]\n", stat);
    }
    return stat;
}

hpm_stat_t ecat_flash_eeprom_read(uint32_t addr, uint8_t* data) {
    hpm_stat_t stat;
    stat = e2p_read(addr, EEPROM_WRITE_SIZE, data);
    if (stat == status_success) {
        e2p_info("\n READ success: addr[%d] - val[0x%x]\n", addr, *((uint16_t*)data));
    } else {
        e2p_info("\n READ failed: error code[%d]\n", stat);
    }
    return stat;
}

void ecat_flash_eeprom_format(void) {
    /* e2p_clear() erases both areas. Nothing in this image calls it -- the symbol
     * exists for the SDK header's prototype set -- but it is left functional
     * rather than stubbed: outside the boot window every erase inside it is
     * refused by ecat_flash_rpc_erase(), so the guard is the same one that
     * protects every other path. */
    uint32_t level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    e2p_clear();
    restore_global_irq(level);
}

void ecat_flash_eeprom_flush(void) {
    /* ../../CORE_SWAP_MIGRATION.md section 3.2: the e2p garbage collector must
     * never run on the data plane. It rewrites every valid entry and then erases
     * a whole 32 KiB half-area, which on core0 is eight sector erases back to
     * back. Refused explicitly here rather than left to the erase gate, so the
     * intent is visible at the call site a future maintainer would add. */
    printf("e2p flush refused: GC must not run on the data plane.\n");
}

void ecat_flash_eeprom_show_info(void) { e2p_show_info(); }
