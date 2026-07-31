#ifndef FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_DIAG_H
#define FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_DIAG_H

#include <stddef.h>

/*
 * printf replacement for the EtherCAT core1 probe image.
 *
 * WHY (see ../../CORE_SWAP_MIGRATION.md section 3.3): a single printf on core1
 * is fatal and undiagnosable. hpm_debug_console.c keeps its UART pointer in a
 * static that only board_init_console() sets, and board_init_core1() never
 * calls it, so g_console_uart stays NULL. uart_send_byte(NULL, ...) then reads
 * 0x34 and writes 0x20 -- and on HPM6E80 the local address range
 * 0x00000000..0x0003FFFF is core0's ILM (hpm_misc.h: CORE0_ILM_LOCAL_BASE = 0,
 * CORE1_ILM_LOCAL_BASE = 0x00040000), with core0's .vectors sitting exactly at
 * 0x00000000. The result is either a spin on a THR-empty bit that never comes
 * or a corrupted core0 interrupt vector table.
 *
 * This header is FORCE-INCLUDED into every C translation unit of the image
 * (-include, see ../CMakeLists.txt) and the macro at the bottom redirects every
 * printf call site -- ours and the SDK EtherCAT port layer's 12 error/first-boot
 * ones -- into the SHARE_RAM diagnostic ring. core0 drains it with
 * xcore_diag_drain() and prints the bytes on its own console.
 *
 * Lossy by design (../../common/xcore_diag.hpp): a full ring drops the
 * remainder of a line rather than stalling core1. A log message must never
 * block the EtherCAT data path.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* The ring is bound by ecat_xcore_init() (ecat_xcore.h), which is where the
 * channel becomes available. Log calls made before that returns are silently
 * dropped; the window is deliberately tiny -- core1 claims the channel before
 * anything else can log.
 *
 * Formatted log line. Supports %s %d %i %u %x %X %c %% plus an optional zero-
 * or space-padded minimum field width (e.g. %08x), which is everything the port
 * layer and this project's own call sites use. NOT a printf: no floating point,
 * no length modifiers, no precision -- see the rationale in ecat_diag.c.
 *
 * Returns the number of bytes accepted by the ring, short of the formatted
 * length when the ring was full.
 *
 * Safe from any context on core1: the ring is SPSC and core1 is its only
 * producer, and the formatter uses one fixed stack buffer, so it is callable
 * from the PDI ISR too.
 *
 * NOTE: the format attribute is spelled __printf__ deliberately -- the plain
 * spelling would be eaten by the macro at the bottom of this header. */
__attribute__((format(__printf__, 1, 2))) int ecat_diag_printf(const char* format, ...);

/* Raw byte sink. Implemented in ecat_xcore.cpp, not ecat_diag.c: the ring
 * producer (xcore_diag_write) is a C++ inline in ../../common/xcore_diag.hpp, so
 * exactly one C++ translation unit owns all shared-memory access and the rest of
 * the image stays plain C. It also backs the strong _write() override in
 * ecat_diag.c, which catches anything still reaching newlib stdio (an assert, or
 * a unit that undefined the macro below). */
void ecat_diag_write(const char* text, size_t size);

#ifdef __cplusplus
}
#endif

/* The redirect itself. Placed after the declaration so this header is also the
 * one that makes the replacement visible -- a bare -Dprintf=... would leave
 * every SDK call site with an implicit declaration. <stdio.h> included later by
 * SDK sources simply declares ecat_diag_printf a second time, compatibly. */
#ifndef printf
# define printf ecat_diag_printf
#endif

#endif /* FIRMWARE_RMCS_BOARD_ECAT_CORE1_ECAT_SRC_ECAT_DIAG_H */
