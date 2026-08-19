cmake_minimum_required(VERSION 3.21...3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

project(rmcs_board_superbuild LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/librmcs_firmware.cmake")

# hpm5321 builds one image serving both HPM5321 PCBs -- single-CAN and dual-CAN --
# selecting how many CAN ports to bring up, and its LED tables, at run time from
# OTP word 25
# (common/board_identity.hpp).
set(BOARD "hpm5321" CACHE STRING "Target board (hpm5321)")
set_property(CACHE BOARD PROPERTY STRINGS hpm5321)
set(BOARD_SEARCH_PATH "${CMAKE_CURRENT_SOURCE_DIR}/boards" CACHE PATH "Board search path")

if(BOARD STREQUAL "hpm5321_dual_can")
    message(FATAL_ERROR
        "hpm5321_dual_can was merged into hpm5321; build with -DBOARD=hpm5321")
endif()
# Bitmanip is enabled by the HPM SoC CMake via the granular _zba_zbb_zbc_zbs
# extensions, so the umbrella "b" here is redundant. Keeping it makes the arch
# string "rv32imafdcb_..._zba_zbb_zbc_zbs", which recent binutils reject ("cannot
# find default versions of the ISA extension `b'"). Drop the redundant "b".
#
# Soft-float on purpose (was rv32imafdc / ilp32d): the firmware and core use no
# floating point anywhere, yet with a hard-float ABI the SDK's preemptive ISR
# wrapper saves/restores 20 caller-saved FP registers as doubles on EVERY
# interrupt entry/exit (SAVE_FPU_CONTEXT in hpm_interrupt.h) -- pure forwarding
# latency spent on nothing. Without F/D, __riscv_flen is undefined and the FPU
# context code vanishes. Revert if on-board float math ever becomes necessary.
set(RV_ARCH "rv32imac" CACHE STRING "RISC-V arch")
set(RV_ABI "ilp32" CACHE STRING "RISC-V ABI")
set(BOARD_BOOTLOADER_MODE "auto" CACHE STRING "Bootloader mode (auto / manual)")
set_property(CACHE BOARD_BOOTLOADER_MODE PROPERTY STRINGS auto manual)

# Core-swap layout (ecat/CORE_SWAP_MIGRATION.md): build the EtherCAT stack as a
# core1 secondary-core image and have the app load and release it at boot. OFF
# builds the plain single-core USB firmware, which is what the USB/CAN
# regressions measure and the only configuration that exists on a tree without
# the core1 EtherCAT project.
option(LIBRMCS_RELEASE_CORE1 "Build the EtherCAT core1 image and release CPU1 from the app" OFF)

# Forwarded to the app sub-build as LIBRMCS_APP_CAN_DIAG; see app/CMakeLists.txt.
option(LIBRMCS_CAN_DIAG "Emit CAN forwarding telemetry on the UART0 uplink" OFF)

# Forwarded to the app sub-build as LIBRMCS_APP_TIME_SYNC; see app/CMakeLists.txt.
option(LIBRMCS_TIME_SYNC "Run the shared USB-SOF time base" OFF)

# Forwarded to the app sub-build as LIBRMCS_APP_PULSE_TEST; see app/CMakeLists.txt.
option(LIBRMCS_PULSE_TEST "Measure cross-board skew with GPTMR pulses (takes over UART0)" OFF)

# Forwarded to the app sub-build as LIBRMCS_APP_SOF_DIAG; see app/CMakeLists.txt.
option(LIBRMCS_SOF_DIAG "Emit USB SOF / FRINDEX validation telemetry on the UART0 uplink" OFF)

# Forwarded to the app sub-build as LIBRMCS_APP_DIAG_OVER_USB.
option(LIBRMCS_DIAG_OVER_USB "Relay core1 diagnostics to the host over USB" OFF)

# Forwarded to the app sub-build as LIBRMCS_APP_ILM_HOT_PATH. HPM5321 always
# uses its board-owned linker layout; this switch applies to HPM6E8Y.
option(LIBRMCS_ILM_HOT_PATH "Run the HPM6E8Y CAN driver from ILM" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "debug")
endif()

librmcs_setup_project_context(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

function(add_rmcs_board_subbuild target_name source_subdir)
    set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}/${source_subdir}")
    set(binary_dir "${CMAKE_CURRENT_BINARY_DIR}/${source_subdir}")
    set(compile_commands_file "${binary_dir}/compile_commands.json")

    set(configure_args
        -G "${CMAKE_GENERATOR}"
        -S "${source_dir}"
        -B "${binary_dir}"
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DLIBRMCS_RMCS_BOARD_SUPERBUILDING=ON
        -UBOARD_SEARCH_DIR
        -USDK_BOARD_DIR
        -DBOARD=${BOARD}
        -DBOARD_SEARCH_PATH=${BOARD_SEARCH_PATH}
        -DRV_ARCH=${RV_ARCH}
        -DRV_ABI=${RV_ABI}
        -DHOST_DEBUGGER=${HOST_DEBUGGER}
        -DSOURCE_PATH_MAP=${LIBRMCS_SOURCE_PATH_MAP}
        -DLIBRMCS_PROJECT_VERSION=${LIBRMCS_PROJECT_VERSION}
    )
    if(CMAKE_MAKE_PROGRAM)
        list(APPEND configure_args -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
    endif()
    list(APPEND configure_args ${ARGN})

    add_custom_target(
        ${target_name}
        BYPRODUCTS "${compile_commands_file}"
        COMMAND ${CMAKE_COMMAND} ${configure_args}
        COMMAND ${CMAKE_COMMAND} --build "${binary_dir}"
        USES_TERMINAL
        VERBATIM
    )
endfunction()

# Where the core1 sub-build must deposit its C-array image, and where the app
# project expects to find it. Single definition so the two cannot drift.
set(LIBRMCS_SEC_CORE_IMG_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/app/generated/sec_core_img.c")

if(LIBRMCS_RELEASE_CORE1)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ecat/core1_ecat/CMakeLists.txt")
        message(FATAL_ERROR
            "LIBRMCS_RELEASE_CORE1=ON but ecat/core1_ecat does not exist yet.\n"
            "That project is the EtherCAT-on-core1 image; until it lands, configure with\n"
            "-DLIBRMCS_RELEASE_CORE1=OFF (the default) to build the single-core USB firmware.\n"
            "Background: firmware/rmcs_board/ecat/CORE_SWAP_MIGRATION.md section 4, step 0.")
    endif()

    # SEC_CORE_IMG_C_ARRAY_OUTPUT is the HPM SDK's own variable for the emitted
    # array; passing it from here keeps the drop point owned by the superbuild
    # rather than hard-coded on both sides.
    add_rmcs_board_subbuild(
        rmcs_board_core1_ecat
        ecat/core1_ecat
        -DSEC_CORE_IMG_C_ARRAY_OUTPUT=${LIBRMCS_SEC_CORE_IMG_OUTPUT}
    )
endif()

add_rmcs_board_subbuild(
    rmcs_board_app
    app
    -DLIBRMCS_APP_RELEASE_CORE1=${LIBRMCS_RELEASE_CORE1}
    -DLIBRMCS_APP_CAN_DIAG=${LIBRMCS_CAN_DIAG}
    -DLIBRMCS_APP_TIME_SYNC=${LIBRMCS_TIME_SYNC}
    -DLIBRMCS_APP_PULSE_TEST=${LIBRMCS_PULSE_TEST}
    -DLIBRMCS_APP_SOF_DIAG=${LIBRMCS_SOF_DIAG}
    -DLIBRMCS_APP_DIAG_OVER_USB=${LIBRMCS_DIAG_OVER_USB}
    -DLIBRMCS_APP_ILM_HOT_PATH=${LIBRMCS_ILM_HOT_PATH}
)
if(LIBRMCS_RELEASE_CORE1)
    # The app compiles the sec_core_img.c that the core1 project emits, so the
    # ordering is a hard requirement, not an optimization. Note that the app's
    # existence check for that file runs at ITS configure time, which this
    # dependency places after the core1 build.
    add_dependencies(rmcs_board_app rmcs_board_core1_ecat)
endif()

add_rmcs_board_subbuild(
    rmcs_board_bootloader
    bootloader
    -DBOARD_BOOTLOADER_MODE=${BOARD_BOOTLOADER_MODE}
)

set(RMCS_BOARD_APP_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/app/compile_commands.json")
set(RMCS_BOARD_BOOTLOADER_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/bootloader/compile_commands.json")
set(RMCS_BOARD_MERGED_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json")

set(RMCS_BOARD_COMPILE_COMMANDS_INPUTS
    "${RMCS_BOARD_APP_COMPILE_COMMANDS}"
    "${RMCS_BOARD_BOOTLOADER_COMPILE_COMMANDS}"
)
set(RMCS_BOARD_COMPILE_COMMANDS_TARGETS rmcs_board_app rmcs_board_bootloader)
if(LIBRMCS_RELEASE_CORE1)
    list(APPEND RMCS_BOARD_COMPILE_COMMANDS_INPUTS
        "${CMAKE_CURRENT_BINARY_DIR}/ecat/core1_ecat/compile_commands.json")
    list(APPEND RMCS_BOARD_COMPILE_COMMANDS_TARGETS rmcs_board_core1_ecat)
endif()

add_custom_command(
    OUTPUT "${RMCS_BOARD_MERGED_COMPILE_COMMANDS}"
    COMMAND
    ${CMAKE_COMMAND}
    "-DOUTPUT_FILE=${RMCS_BOARD_MERGED_COMPILE_COMMANDS}"
    "-DINPUT_FILES=${RMCS_BOARD_COMPILE_COMMANDS_INPUTS}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/merge_compile_commands.cmake"
    DEPENDS
    ${RMCS_BOARD_COMPILE_COMMANDS_TARGETS}
    ${RMCS_BOARD_COMPILE_COMMANDS_INPUTS}
    COMMENT "Merging rmcs_board compile commands"
    VERBATIM
)

add_custom_target(
    rmcs_board_compile_commands
    DEPENDS "${RMCS_BOARD_MERGED_COMPILE_COMMANDS}"
)

add_custom_target(rmcs_board ALL DEPENDS rmcs_board_compile_commands)
