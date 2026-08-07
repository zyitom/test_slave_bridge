# Core0 (EtherCAT core) image of the RMCS EtherCAT stream bridge.
#
# Owns the integrated ESC and the Beckhoff SSC stack, bridges the process
# data stream chunks to the fieldbus core through the SHARE_RAM rings, and
# loads/releases the core1 image at boot. Build ../core1 FIRST -- it emits
# src/sec_core_img.c consumed here -- or use the ecat superbuild
# (../CMakeLists.txt), which orders everything.
#
# Linked as a flash_uf2 image at offset 0x20000 behind the shared rmcs_board
# bootloader (../../bootloader), exactly like the USB app; a .dfu artifact is
# produced for bootloader-based flashing.

cmake_minimum_required(VERSION 3.21...3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if("${HPM_BUILD_TYPE}" STREQUAL "")
    set(HPM_BUILD_TYPE flash_uf2)
endif()

set(BOARD "hpm6e80ivm1" CACHE STRING "Target board")
set(BOARD_SEARCH_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../boards" CACHE PATH "Board search path")

# Native CAN mailbox variant: swaps pd_glue.cpp (ARQ byte stream) for
# native_glue.cpp (four per-bus mailboxes over the same 48-byte PDO).
option(RMCS_ECAT_NATIVE_CAN "Build the native CAN mailbox variant" OFF)
# Hybrid fixed-PDO variant: swaps pd_glue.cpp for hybrid_glue.cpp (28 x 12-byte
# cyclic CAN slots + a 16-byte pd_stream chunk over a 352-byte PDO).
option(RMCS_ECAT_HYBRID_PD "Build the hybrid fixed-PDO variant" OFF)

if(RMCS_ECAT_NATIVE_CAN AND RMCS_ECAT_HYBRID_PD)
    message(FATAL_ERROR "RMCS_ECAT_NATIVE_CAN and RMCS_ECAT_HYBRID_PD are mutually exclusive.")
endif()

# Enables the SDK flash-EEPROM-emulation component (must precede find_package).
set(CONFIG_EEPROM_EMULATION 1)
set(CONFIG_USB_DEVICE 1)

# Hot-path linker script: EtherCAT data-path objects execute from ILM instead
# of XIP flash (see the .ld header). Must precede find_package.
if("${HPM_BUILD_TYPE}" STREQUAL "flash_uf2")
    set(CUSTOM_GCC_LINKER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/hpm6e80_flash_uf2_ilm_hot.ld")
endif()

set(ENV{HPM_SDK_BASE} "${CMAKE_CURRENT_SOURCE_DIR}/../../bsp/hpm_sdk")
find_package(hpm-sdk REQUIRED HINTS "$ENV{HPM_SDK_BASE}")

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../common/cmake/librmcs_firmware.cmake")
librmcs_setup_project_context(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

project(rmcs_ecat_core0 LANGUAGES C CXX ASM)

set(APP_NAME "rmcs_ecat_bridge_${BOARD}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Beckhoff SSC sources are license-gated (ETG membership) and are NOT
# vendored in this repository. Generate them with the SSC Tool and place them
# under SSC/Src; see ../README.md for the exact steps.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SSC/Src/ecatslv.c")
    message(FATAL_ERROR
        "Beckhoff SSC sources not found at ecat/core0/SSC/Src.\n"
        "Generate them with the Beckhoff SSC Tool (ETG membership required), then run\n"
        "ecat/tools/import_ssc.sh. Exact steps: firmware/rmcs_board/ecat/README.md.")
endif()

# import_ssc.sh installs the stream-bridge object dictionary and SII over the
# generated files; a raw SSC Tool output would build but describe 4-byte
# counter PDOs. Catch that before compiling.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/SSC/Src/digital_ioObjects.h" _rmcs_od_header)
if(NOT _rmcs_od_header MATCHES "RMCS_STREAM_OBJECTS")
    message(FATAL_ERROR
        "SSC/Src contains the raw SSC Tool output (stock object dictionary).\n"
        "Run ecat/tools/import_ssc.sh <generated-Src-dir> to install the stream-bridge\n"
        "overrides. Steps: firmware/rmcs_board/ecat/README.md.")
endif()
unset(_rmcs_od_header)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sec_core_img.c")
    message(FATAL_ERROR
        "src/sec_core_img.c missing: build ../core1 first (it generates the core1 image\n"
        "array consumed by this project). Steps: firmware/rmcs_board/ecat/README.md.")
endif()

# Process data buffer sizes for the SSC (guarded by #ifndef in the generated
# ecat_def.h). Must equal RMCS_PD_CHUNK_SIZE (src/rmcs_pd.h) -- ecat_appl.c
# static-asserts it. Applied SDK-wide so every stack translation unit sees the
# same buffer layout. Stock/native: one 48-byte chunk (0x30). Hybrid: a
# 352-byte PDO (0x160) = 336-byte cyclic region + 16-byte stream chunk.
if(RMCS_ECAT_HYBRID_PD)
    sdk_compile_definitions(-DMAX_PD_INPUT_SIZE=0x160 -DMAX_PD_OUTPUT_SIZE=0x160)
    sdk_compile_definitions(-DRMCS_ECAT_HYBRID_PD=1)
else()
    sdk_compile_definitions(-DMAX_PD_INPUT_SIZE=0x30 -DMAX_PD_OUTPUT_SIZE=0x30)
endif()
sdk_compile_definitions(-DLIBRMCS_PROJECT_VERSION_STRING="${LIBRMCS_PROJECT_VERSION}")
sdk_compile_definitions(-DCFG_TUSB_MCU=OPT_MCU_HPM)
sdk_compile_definitions(-DUSB_HOST_MCU_CORE=HPM_CORE0)
sdk_inc("${CMAKE_CURRENT_SOURCE_DIR}/include")

# Beckhoff SSC (generated).
sdk_inc(SSC/Src)
sdk_src(SSC/Src/coeappl.c)
if(RMCS_ECAT_HYBRID_PD)
    # ecatappl.c is the only translation unit that #include "eeprom.h" (the SII
    # image byte array). It is a quoted include resolved relative to the file's
    # OWN directory (SSC/Src), so a prepended -I cannot override it. To install
    # the hybrid SII (revision 5, 352-byte SMs) WITHOUT editing the tracked SSC
    # sources, compile a build-tree COPY of ecatappl.c from a staging directory
    # that also holds a copy of the hybrid eeprom named eeprom.h: same-directory
    # resolution then picks up the hybrid image, while every other quoted SSC
    # include (ecatappl.h, digital_io.h, ...) still resolves via sdk_inc(SSC/Src).
    #
    # ROLLBACK TRAP (device side): the port refreshes its emulated EEPROM only
    # when the built-in revision is GREATER than the stored one. Once a board has
    # booted the hybrid image (revision 5), reverting to the stock stream image
    # (revision 3) will NOT refresh -- bump the stock REVISION above 5 in
    # tools/patch_sii.py and regenerate SSC/Src/eeprom.h before reverting.
    set(_rmcs_hybrid_ssc "${CMAKE_CURRENT_BINARY_DIR}/hybrid_ssc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/ssc_overrides/eeprom_hybrid.h"
        "${_rmcs_hybrid_ssc}/eeprom.h" COPYONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/SSC/Src/ecatappl.c"
        "${_rmcs_hybrid_ssc}/ecatappl.c" COPYONLY)
    sdk_src("${_rmcs_hybrid_ssc}/ecatappl.c")
else()
    sdk_src(SSC/Src/ecatappl.c)
endif()
sdk_src(SSC/Src/ecatcoe.c)
sdk_src(SSC/Src/ecatslv.c)
sdk_src(SSC/Src/mailbox.c)
sdk_src(SSC/Src/objdef.c)
sdk_src(SSC/Src/sdoserv.c)

# SDK EtherCAT port layer (ESC bring-up, PDI/SYNC ISRs, PHY, EEPROM emulation).
set(ECAT_PORT_DIR ${HPM_SDK_BASE}/samples/ethercat/port)
sdk_inc(${ECAT_PORT_DIR})
sdk_src(${ECAT_PORT_DIR}/hpm_ecat_hw.c)
sdk_src(${ECAT_PORT_DIR}/hpm_ecat_phy.c)
if(CONFIG_EEPROM_EMULATION)
    sdk_src(${ECAT_PORT_DIR}/hpm_ecat_e2p_emulation.c)
endif()

# ecat_config.h / user_config.h must be visible to the port layer above
# (compiled into the SDK library target), hence sdk_inc rather than app-only.
sdk_inc(src)

# ../../../../core/include: shared librmcs headers (the pd_stream chunk
# layout lives there because the host SOEM transport uses the same code).
sdk_app_inc(src ../common ../../../../core/include ${HPM_SDK_BASE}/samples/multicore/common)
sdk_app_inc("${LIBRMCS_PROJECT_ROOT}")
sdk_app_src(${HPM_SDK_BASE}/samples/multicore/common/multicore_common.c)
sdk_app_src(src/ecat_main.c)
sdk_app_src(src/ecat_appl.c)
if(RMCS_ECAT_NATIVE_CAN)
    sdk_compile_definitions(-DRMCS_ECAT_NATIVE_CAN=1)
    sdk_app_src(src/native_glue.cpp)
elseif(RMCS_ECAT_HYBRID_PD)
    # RMCS_ECAT_HYBRID_PD is already defined above.
    sdk_app_src(src/hybrid_glue.cpp)
else()
    sdk_app_src(src/pd_glue.cpp)
endif()
sdk_app_src(src/sec_core_img.c)
sdk_app_src(src/usb_runtime.cpp)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/current_tinyusb.cmake")
librmcs_add_current_tinyusb(
    src/class/vendor/vendor_device.c
    src/class/dfu/dfu_rt_device.c
)

set_property(TARGET app PROPERTY CXX_STANDARD 23)
set_property(TARGET app PROPERTY CXX_STANDARD_REQUIRED ON)

# Leave room for the shared rmcs_board bootloader (flash_xip at 0x80000000,
# 124 KiB + 4 KiB metadata); the app image starts at flash offset 0x20000.
if("${HPM_BUILD_TYPE}" STREQUAL "flash_uf2")
    sdk_linker_global_symbols("_uf2_bl_length=0x20000")

    get_property(LIBRMCS_USB_PID GLOBAL PROPERTY LIBRMCS_USB_PID)
    if("${LIBRMCS_USB_PID}" STREQUAL "")
        message(FATAL_ERROR "LIBRMCS_USB_PID must be set by the selected board")
    endif()

    librmcs_add_dfu_image(
        TARGET ${APP_ELF_NAME}
        INPUT_BINARY "${EXECUTABLE_OUTPUT_PATH}/${APP_BIN_NAME}"
        OUTPUT_DFU "${EXECUTABLE_OUTPUT_PATH}/${APP_NAME}.dfu"
        VENDOR_ID 0xA11C
        PRODUCT_ID ${LIBRMCS_USB_PID}
        DEVICE_ID 0x0300
        COMMENT "Generating ${APP_NAME}.dfu"
    )
endif()

generate_ide_projects()
