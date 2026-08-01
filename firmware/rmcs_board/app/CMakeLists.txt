cmake_minimum_required(VERSION 3.21...3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Fold the selected board into the output name so the produced ELF/BIN/DFU make
# the target board obvious (e.g. rmcs_board_app_hpm5321.elf vs
# rmcs_board_app_hpm5321_dual_can.elf) and the wrong image cannot be flashed by
# mistake.
set(APP_NAME "rmcs_board_app_${BOARD}")

if(NOT LIBRMCS_RMCS_BOARD_SUPERBUILDING)
    message(FATAL_ERROR
        "Do not configure firmware/rmcs_board/app directly.\n"
        "Use the rmcs_board superbuild instead:\n")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/librmcs_firmware.cmake")

set(HPM_BUILD_TYPE "flash_uf2" CACHE STRING "HPM build type")

# Run the CAN forwarding leaf calls from ILM instead of FLASH XIP. Our own glue
# is already in .fast via ATTR_PLACE_AT, but the SDK's MCAN driver -- called on
# every single frame -- is not, so the hot path still takes XIP fetches.
#
# Worth it on the EtherCAT path and not on USB: both transports quantize the
# round trip, but EtherCAT's quantum is the 25.75 us cycle while USB's is the
# 125 us microframe, so a microsecond saved here has ~5x the chance of dropping
# a whole quantum. Evidence: HOST_TUNING.md section 4.2 and
# ecat/DESIGN.md section 4.3.
#
# OFF keeps the stock SDK script, which is the configuration every measurement
# before 2026-08-01 was taken on.
option(LIBRMCS_APP_ILM_HOT_PATH "Run the CAN driver from ILM (custom linker script)" OFF)
if(LIBRMCS_APP_ILM_HOT_PATH)
    set(CUSTOM_GCC_LINKER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/hpm6e80_flash_uf2_ilm_hot.ld")
endif()

# Load the EtherCAT core1 image and release CPU1 at boot (core-swap layout, see
# ecat/CORE_SWAP_MIGRATION.md). OFF keeps this a self-contained single-core USB
# firmware, which is the configuration the USB and CAN regression tests run
# against -- and the only one buildable before the core1 project exists.
option(LIBRMCS_APP_RELEASE_CORE1 "Load and release the EtherCAT core1 image" OFF)

# CAN forwarding telemetry (src/diag/can_diag.*): ISR entry counts, MCAN status
# registers and the PLIC pending/enable arrays, sampled from the main loop and
# shipped out as UART0 uplink frames. Used to diagnose the high-load forwarding
# stall of ecat/CORE_SWAP_MIGRATION.md section 6 on a board with no JTAG or
# serial console attached. OFF keeps the CAN hot path free of the counters.
option(LIBRMCS_APP_CAN_DIAG "Emit CAN forwarding telemetry on the UART0 uplink" OFF)

# Relay core1's diagnostic ring to the host as UART0 uplink frames instead of to
# the board console. The console is a UART on the FT2232 debug header, which is
# not populated on the boards this runs on, so without this core1's boot log --
# including everything the emulated EEPROM reports about a first-boot SII
# rewrite -- has no reachable sink. OFF by default because DataId::kUart0 is a
# real data port here (UART1) and must not carry log text behind a host's back.
option(LIBRMCS_APP_DIAG_OVER_USB "Relay core1 diagnostics to the host over USB" OFF)

set(CONFIG_DMA_MGR 1)
set(CONFIG_TINYUSB 1)
set(CONFIG_USB_DEVICE 1)
set(CONFIG_USB_DEVICE_VENDOR 1)
set(LIBRMCS_BUILDING_FIRMWARE_APP 1)

# Always use the SDK pinned by this project's bsp/hpm_sdk submodule, ignoring any
# external HPM_SDK_BASE in the environment, so the build is reproducible.
set(ENV{HPM_SDK_BASE} "${CMAKE_CURRENT_SOURCE_DIR}/../bsp/hpm_sdk")
find_package(hpm-sdk REQUIRED HINTS "$ENV{HPM_SDK_BASE}")

get_property(LIBRMCS_USB_PID GLOBAL PROPERTY LIBRMCS_USB_PID)
if("${LIBRMCS_USB_PID}" STREQUAL "")
    message(FATAL_ERROR "LIBRMCS_USB_PID must be set by the selected board")
endif()

librmcs_setup_project_context(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

project(rmcs_board_app VERSION 3 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

sdk_compile_options(-Wall -Wextra -Wpedantic -fvisibility=hidden)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
sdk_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)
if(LIBRMCS_SOURCE_PATH_MAP_COMPILE_OPTIONS)
    sdk_compile_options(${LIBRMCS_SOURCE_PATH_MAP_COMPILE_OPTIONS})
endif()

if(CMAKE_BUILD_TYPE STREQUAL "debug")
    sdk_compile_options(-Og)
endif()

if(LIBRMCS_APP_CAN_DIAG)
    sdk_compile_definitions(-DLIBRMCS_APP_CAN_DIAG=1)
endif()
if(LIBRMCS_APP_DIAG_OVER_USB)
    sdk_compile_definitions(-DLIBRMCS_APP_DIAG_OVER_USB=1)
endif()

sdk_compile_definitions(-DLIBRMCS_PROJECT_VERSION_STRING="${LIBRMCS_PROJECT_VERSION}")
sdk_compile_definitions(-DCFG_TUSB_MCU=OPT_MCU_HPM)
sdk_compile_definitions(-DUSB_HOST_MCU_CORE=HPM_CORE0)

sdk_inc("${CMAKE_CURRENT_SOURCE_DIR}/include")
sdk_app_inc("${LIBRMCS_PROJECT_ROOT}/core/include")
sdk_app_inc("${LIBRMCS_PROJECT_ROOT}")

file(GLOB_RECURSE PROJECT_SOURCE CONFIGURE_DEPENDS
    "${LIBRMCS_PROJECT_ROOT}/core/src/*.cpp"
    "${LIBRMCS_PROJECT_ROOT}/core/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)
sdk_app_src(${PROJECT_SOURCE})
sdk_app_src("$ENV{HPM_SDK_BASE}/middleware/tinyusb/src/class/dfu/dfu_rt_device.c")

if(LIBRMCS_APP_RELEASE_CORE1)
    # The generated core1 image lives in generated/, NOT in src/: the GLOB_RECURSE
    # above would otherwise swallow it, and a ~500 KB machine-generated C array
    # sitting among the hand-written sources invites someone to edit or review it.
    set(LIBRMCS_SEC_CORE_IMG "${CMAKE_CURRENT_SOURCE_DIR}/generated/sec_core_img.c")
    if(NOT EXISTS "${LIBRMCS_SEC_CORE_IMG}")
        message(FATAL_ERROR
            "LIBRMCS_APP_RELEASE_CORE1=ON but app/generated/sec_core_img.c is missing.\n"
            "It is emitted by the EtherCAT core1 sub-build (ecat/core1_ecat), which the\n"
            "rmcs_board superbuild runs before this project. Either build through the\n"
            "superbuild (firmware/rmcs_board/CMakeLists.txt) or configure with\n"
            "-DLIBRMCS_APP_RELEASE_CORE1=OFF for a single-core image.\n"
            "Background: firmware/rmcs_board/ecat/CORE_SWAP_MIGRATION.md section 4, step 0.")
    endif()

    sdk_compile_definitions(-DLIBRMCS_APP_RELEASE_CORE1=1)

    # --- Detach the USB data plane (migration step 1) -------------------------
    #
    # Step 1 hard-codes "EtherCAT always owns the data plane" so that correctness
    # and arbitration can be validated separately. The PD stream
    # (src/xcore/pd_link.cpp) therefore becomes the one live link::HostSession,
    # and src/usb/vendor.cpp's three data-plane bindings have to go:
    #
    #   * link::uplink_serializer / link::uplink_enabled -- strong definitions
    #     pointing at usb::vendor. pd_link.cpp defines the same two symbols, so
    #     leaving both in the link is a duplicate-symbol error. Renaming them
    #     here makes vendor.cpp's pair unreferenced and lets the CAN/UART
    #     drivers, which know the protocol stack ONLY through these two free
    #     functions, reach the PD stream with zero source changes.
    #   * tud_vendor_rx_cb -- the USB bulk OUT hook. Not renaming it would leave
    #     a real hole rather than a cosmetic one: usb::vendor is a second
    #     HostSession with its own deserializer, so a USB host could still
    #     establish a session and drive the CAN buses while EtherCAT believes it
    #     owns the link. With the strong definition renamed away, TinyUSB's weak
    #     no-op stub takes over; vendor_device.c re-arms the RX endpoint
    #     unconditionally right after the callback and CFG_TUD_VENDOR_RX_BUFSIZE
    #     is 0, so OUT packets are drained and dropped with no stall and no FIFO
    #     growth.
    #
    # What deliberately KEEPS working, because it lives in the same file and is
    # untouched: tusb_rhport_init + the USB0 ISR (enumeration), the descriptor
    # callbacks, and tud_dfu_runtime_reboot_to_dfu_cb + poll_dfu_runtime_reboot
    # (DFU-RT). Breaking those would break the flashing path, which is the whole
    # reason this migration moves EtherCAT instead of USB.
    #
    # vendor.cpp guards those three bindings on LIBRMCS_APP_RELEASE_CORE1
    # directly. An earlier revision instead renamed the symbols through a
    # per-source COMPILE_DEFINITIONS list; that also renamed HostSession's inline
    # uplink_enabled() member inside that one translation unit, which is an ODR
    # violation the compiler cannot diagnose. Not worth it to avoid an #if.
    #
    # The cross-core channel headers (ecat/common) are included through the
    # project root, which is already on the include path above.
    sdk_app_inc("$ENV{HPM_SDK_BASE}/samples/multicore/common")
    sdk_app_src("$ENV{HPM_SDK_BASE}/samples/multicore/common/multicore_common.c")
    sdk_app_src("${LIBRMCS_SEC_CORE_IMG}")
endif()

sdk_linker_global_symbols("_uf2_bl_length=0x20000")

set_property(TARGET app PROPERTY CXX_STANDARD 23)
set_property(TARGET app PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET app PROPERTY CXX_EXTENSIONS OFF)

get_target_property(_hpm_incs ${HPM_SDK_LIB_ITF} INTERFACE_INCLUDE_DIRECTORIES)
if(_hpm_incs)
    set_target_properties(${HPM_SDK_LIB_ITF} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_hpm_incs}")
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
