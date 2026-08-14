# EtherCAT-on-core1 probe image (core-swap migration step 0).
#
# A secondary-core image containing ONLY the EtherCAT stack: the Beckhoff SSC,
# the SDK EtherCAT port layer, a read-only copy of the flash-EEPROM emulation,
# and the cross-core diagnostic ring. No USB, no CAN, no protocol stack -- those
# consolidate onto core0 (see ../CORE_SWAP_MIGRATION.md section 4, step 0).
#
# The SDK links it against ram_core1.ld and emits the binary as a C array into
# SEC_CORE_IMG_C_ARRAY_OUTPUT, which the new core0 project compiles in and loads
# into core1's ILM at boot (same pattern as ../core1 and samples/multicore/hello).
# Build this BEFORE the core0 image that embeds it.
#
# Three things this deliberately does NOT copy from ../core1/CMakeLists.txt:
#   * -Wall -Wextra -Wpedantic: the SSC generated sources and the SDK port layer
#     do not survive -Wpedantic (../CORE_SWAP_MIGRATION.md section 5).
#   * CUSTOM_GCC_LINKER_FILE: a secondary-core image uses the SDK's ram_core1.ld
#     and every byte of it already executes from ILM, so ../core0's hand-curated
#     hot-path script has nothing left to do and is retired here.
#   * The TinyUSB sources from ../core0/CMakeLists.txt: USB stays on the boot core.

cmake_minimum_required(VERSION 3.21...3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(HPM_BUILD_TYPE "sec_core_img")
# Points at the new core0 project's source tree, which does not exist yet; the
# directory is tracked with a .gitkeep so the build has somewhere to write.
set(SEC_CORE_IMG_C_ARRAY_OUTPUT
    ${CMAKE_CURRENT_SOURCE_DIR}/../../app/generated/sec_core_img.c)

set(BOARD "hpm6e8y" CACHE STRING "Target board")
set(BOARD_SEARCH_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../boards" CACHE PATH "Board search path")

# Enables the SDK flash-EEPROM-emulation component (must precede find_package).
# Only eeprom_emulation.c and its nor_flash port come from the component; the
# EtherCAT-facing wrapper is this project's read-only copy, see below.
set(CONFIG_EEPROM_EMULATION 1)

# Drop the SDK's default `-u _printf_float` / `-u _scanf_float`. Those two flags
# FORCE-link newlib's float formatting even when nothing references it: it is
# weak inside nano's vfprintf, and the -u is what makes it resident. Measured on
# this image, they cost 12,232 B of ILM (float + soft-double + dtoa + strtod +
# locale) purely so an unreachable %f would work -- and nothing here can print a
# float at all, since ecat_diag_printf does not implement one.
#
# Only the two -u flags are dropped; nano.specs is re-added below, so this is not
# a switch to full newlib (which would be strictly larger). Must precede
# find_package: cmake/toolchain/gcc.cmake reads the variable during it.
set(HPM_SDK_LD_NO_NANO_SPECS 1)

set(ENV{HPM_SDK_BASE} "${CMAKE_CURRENT_SOURCE_DIR}/../../bsp/hpm_sdk")
find_package(hpm-sdk REQUIRED HINTS "$ENV{HPM_SDK_BASE}")

sdk_ld_options("--specs=nano.specs")

project(rmcs_ecat_core1_ecat LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

if(CMAKE_BUILD_TYPE STREQUAL "debug")
    sdk_compile_options(-Og)
endif()

# --- printf eradication (../CORE_SWAP_MIGRATION.md section 3.3) ---------------
#
# Risk 1 on the migration's own list, and the only failure mode there that is
# both certain to happen and impossible to attribute after the fact. A printf on
# core1 dereferences a NULL UART pointer, which on HPM6E80 lands inside core0's
# ILM at core0's .vectors. Three independent measures, per the plan:
#
#   1. E2P_DEBUG_LEVEL = NONE in src/user_config.h compiles out the 62
#      e2p_err/e2p_warn/e2p_info sites in eeprom_emulation.c.
#   2. src/ecat_diag.h is force-included into every C translation unit and
#      #defines printf to ecat_diag_printf, which formats into a stack buffer and
#      pushes into the SHARE_RAM diagnostic ring. This is what catches the SDK
#      port layer's 12 calls in hpm_ecat_hw.c without editing the SDK. The
#      -include also guarantees the replacement is DECLARED at every call site,
#      which a bare -Dprintf=... would not.
#   3. CONFIG_NDEBUG_CONSOLE=1 (below) plus a strong _write() in src/ecat_diag.c.
#
# On measure 3, verified rather than assumed: components/debug_console is added
# UNCONDITIONALLY by the SDK (components/CMakeLists.txt uses a bare
# add_subdirectory, not add_subdirectory_ifdef), so hpm_debug_console.c cannot be
# dropped from the link the way an optional component could -- and board.c calls
# console_init(), so its symbols are genuinely referenced. What CAN be changed is
# WHICH half of that file compiles: the whole UART implementation sits inside
# `#if !defined(CONFIG_NDEBUG_CONSOLE) || !CONFIG_NDEBUG_CONSOLE`. Setting the
# macro to 1 replaces console_init/console_send_byte with no-op stubs AND turns
# its _write() from a strong definition into ATTR_WEAK, which is what lets
# ecat_diag.c's strong _write() override it at link time. That closes the
# remaining hole: a translation unit that includes <cstdio> undefines the printf
# macro, and its call then reaches newlib stdio -> _write -> the diagnostic ring.
sdk_compile_definitions(-DCONFIG_NDEBUG_CONSOLE=1)
sdk_compile_options($<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/src/ecat_diag.h>)

# Process data buffer sizes for the SSC (guarded by #ifndef in the generated
# ecat_def.h). Must equal RMCS_PD_CHUNK_SIZE (src/ecat_appl.c static-asserts it).
# Applied SDK-wide so every stack translation unit sees the same buffer layout.
# Step 0 carries the stock 48-byte chunk only; the hybrid/native variants stay on
# core0 until step 5.
sdk_compile_definitions(-DMAX_PD_INPUT_SIZE=0x30 -DMAX_PD_OUTPUT_SIZE=0x30)

# --- Beckhoff SSC (generated, shared with the core0 project) ------------------
#
# The sources are license-gated (ETG membership) and are NOT vendored in this
# repository. They live in ../core0/SSC and are referenced from there rather than
# duplicated: two copies would drift, and the SII/object-dictionary overrides
# installed by tools/import_ssc.sh are what make the master-visible interface
# identical before and after the migration.
set(RMCS_SSC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core0/SSC/Src")

if(NOT EXISTS "${RMCS_SSC_DIR}/ecatslv.c")
    message(FATAL_ERROR
        "Beckhoff SSC sources not found at ecat/core0/SSC/Src.\n"
        "Generate them with the Beckhoff SSC Tool (ETG membership required), then run\n"
        "ecat/tools/import_ssc.sh. Exact steps: firmware/rmcs_board/ecat/README.md.")
endif()

# import_ssc.sh installs the stream-bridge object dictionary and SII over the
# generated files; a raw SSC Tool output would build but describe 4-byte
# counter PDOs. Catch that before compiling.
file(READ "${RMCS_SSC_DIR}/digital_ioObjects.h" _rmcs_od_header)
if(NOT _rmcs_od_header MATCHES "RMCS_STREAM_OBJECTS")
    message(FATAL_ERROR
        "SSC/Src contains the raw SSC Tool output (stock object dictionary).\n"
        "Run ecat/tools/import_ssc.sh <generated-Src-dir> to install the stream-bridge\n"
        "overrides. Steps: firmware/rmcs_board/ecat/README.md.")
endif()
unset(_rmcs_od_header)

sdk_inc(${RMCS_SSC_DIR})
sdk_src(${RMCS_SSC_DIR}/coeappl.c)
sdk_src(${RMCS_SSC_DIR}/ecatappl.c)
sdk_src(${RMCS_SSC_DIR}/ecatcoe.c)
sdk_src(${RMCS_SSC_DIR}/ecatslv.c)
sdk_src(${RMCS_SSC_DIR}/mailbox.c)
sdk_src(${RMCS_SSC_DIR}/objdef.c)
sdk_src(${RMCS_SSC_DIR}/sdoserv.c)

# FoE + BOOT state. The SSC Tool emits these three only when FOE_SUPPORTED and
# BOOTSTRAPMODE_SUPPORTED are set -- which is the difference between the stock
# ECAT_IO configuration and tools/HPM_ECAT_RMCS_Config.xml.
#
# Detected rather than required, so ONE source tree builds against either
# generation. A repository that still holds a pre-FoE SSC keeps building (with
# firmware update over EtherCAT simply absent); regenerating with the RMCS
# config turns it on with no CMake edit. RMCS_SSC_HAS_FOE is what the sources
# gate on -- never test for the files again outside this block.
if(EXISTS "${RMCS_SSC_DIR}/foeappl.c")
    set(RMCS_SSC_HAS_FOE 1)
    sdk_src(${RMCS_SSC_DIR}/bootmode.c)
    sdk_src(${RMCS_SSC_DIR}/ecatfoe.c)
    sdk_src(${RMCS_SSC_DIR}/foeappl.c)
    message(STATUS "SSC has FoE: firmware update over EtherCAT is enabled")
else()
    set(RMCS_SSC_HAS_FOE 0)
    message(STATUS
        "SSC has no FoE (regenerate with ecat/tools/HPM_ECAT_RMCS_Config.xml to enable)")
endif()
sdk_compile_definitions(-DRMCS_SSC_HAS_FOE=${RMCS_SSC_HAS_FOE})

# --- SDK EtherCAT port layer -------------------------------------------------
#
# hpm_ecat_hw.c and hpm_ecat_phy.c come from the SDK unchanged. Its third file,
# hpm_ecat_e2p_emulation.c, is replaced by src/hpm_ecat_e2p_emulation.c: a
# project copy that drops nor_flash_init() and routes every flash write through
# src/ecat_flash.cpp to core0, because rom_xpi_nor_auto_config() reconfigures the
# XPI0 controller that core0 is executing from (../CORE_SWAP_MIGRATION.md
# section 3.2). The SDK tree stays untouched; only which .c is compiled changes.
# The SDK's own hpm_ecat_e2p_emulation.h is still the header both sides agree on.
set(ECAT_PORT_DIR ${HPM_SDK_BASE}/samples/ethercat/port)
sdk_inc(${ECAT_PORT_DIR})
sdk_src(${ECAT_PORT_DIR}/hpm_ecat_hw.c)
sdk_src(${ECAT_PORT_DIR}/hpm_ecat_phy.c)

# The SDK's generic FoE glue: it holds the file-name/password gate and the block
# sequencing, and reaches the board through four function pointers. Those four
# are where this project diverges from the SDK sample -- the sample programs the
# flash inline, which this core cannot do, so src/ecat_foe_support.cpp routes the
# two writing ones through the MBX1 flash RPC instead. Added here, next to the
# port layer it belongs to, because ECAT_PORT_DIR is only defined above.
if(RMCS_SSC_HAS_FOE)
    # NOT hpm_ecat_foe.c: its hardcoded password locks out IgH masters and it
    # programs the NOR inline, which this core cannot do. src/ecat_foe_support.cpp
    # implements the four SSC callbacks directly instead -- see its header comment.
    sdk_app_src(src/ecat_foe_support.cpp)
endif()
if(CONFIG_EEPROM_EMULATION)
    sdk_src(src/hpm_ecat_e2p_emulation.c)
endif()

# ecat_config.h / user_config.h / ecat_diag.h must be visible to the port layer
# above (compiled into the SDK library target), hence sdk_inc rather than
# app-only.
sdk_inc(src)

# ../../../../core/include: librmcs/ecat/pd_stream.hpp, the ARQ line format
# shared verbatim with the host transport (step 1).
sdk_app_inc(src ../common ../../common ../../../../core/include)
sdk_app_src(src/ecat_main.c)
sdk_app_src(src/ecat_appl.c)
sdk_app_src(src/ecat_diag.c)
sdk_app_src(src/ecat_xcore.cpp)
# Process-data plane: ARQ endpoint + ring hand-off (step 1 of the core swap).
sdk_app_src(src/ecat_pd.cpp)
# Flash-write delegation client (step 3). Deliberately an app source and not an
# sdk_src even though its only caller (src/hpm_ecat_e2p_emulation.c) is compiled
# into the SDK library: it touches SHARE_RAM, and ../common is on the app include
# path only. The cross-library reference resolves the same way ecat_appl.c's
# APPL_* definitions already do.
sdk_app_src(src/ecat_flash.cpp)

set_property(TARGET app PROPERTY CXX_STANDARD 23)
set_property(TARGET app PROPERTY CXX_STANDARD_REQUIRED ON)

generate_ide_projects()
