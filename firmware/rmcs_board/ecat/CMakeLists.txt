cmake_minimum_required(VERSION 3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Superbuild of the RMCS EtherCAT stream bridge (HPM6E80IVM1, dual-core):
#   1. core1      -- fieldbus application, emitted as a C array image
#   2. core0      -- ESC + Beckhoff SSC, embeds the core1 image; flash_uf2
#                    app at offset 0x20000 behind the shared bootloader
#   3. bootloader -- the shared rmcs_board USB-DFU bootloader (flash_xip)
#
# Mirrors the rmcs_board app+bootloader superbuild so the flash layout, DFU
# artifacts and force-stay/upgrade story are identical across boards.

project(rmcs_ecat_superbuild LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/librmcs_firmware.cmake")

set(BOARD "hpm6e80ivm1" CACHE STRING "Target board")
set(BOARD_SEARCH_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../boards" CACHE PATH "Board search path")
# Same soft-float reasoning as the rmcs_board superbuild: no float anywhere in
# the bridge firmware, and a hard-float ABI would make every ISR entry/exit
# save 20 FP registers (see firmware/rmcs_board/CMakeLists.txt).
set(RV_ARCH "rv32imac" CACHE STRING "RISC-V arch")
set(RV_ABI "ilp32" CACHE STRING "RISC-V ABI")
set(BOARD_BOOTLOADER_MODE "auto" CACHE STRING "Bootloader mode (auto / manual)")
set_property(CACHE BOARD_BOOTLOADER_MODE PROPERTY STRINGS auto manual)
option(RMCS_ECAT_CORE1_LOOPBACK "Build the P1 loopback bring-up image on core1" OFF)
option(RMCS_ECAT_CORE1_CAN_PIN_SCANNER "Build the HPM6E8Y CAN pin scanner on core1" OFF)
option(RMCS_ECAT_CORE1_LED_PIN_SCANNER "Build the HPM6E8Y LED GPIO pin scanner on core1" OFF)
option(RMCS_ECAT_CORE1_LED_CONFIRM "Build the HPM6E8Y known-LED confirmation image on core1" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "debug")
endif()

librmcs_setup_project_context(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

function(add_rmcs_ecat_subbuild target_name source_dir)
    set(binary_dir "${CMAKE_CURRENT_BINARY_DIR}/${target_name}")
    set(compile_commands_file "${binary_dir}/compile_commands.json")

    set(configure_args
        -G "${CMAKE_GENERATOR}"
        -S "${source_dir}"
        -B "${binary_dir}"
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
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

add_rmcs_ecat_subbuild(
    rmcs_ecat_core1
    "${CMAKE_CURRENT_SOURCE_DIR}/core1"
    -DRMCS_ECAT_CORE1_LOOPBACK=${RMCS_ECAT_CORE1_LOOPBACK}
    -DRMCS_ECAT_CORE1_CAN_PIN_SCANNER=${RMCS_ECAT_CORE1_CAN_PIN_SCANNER}
    -DRMCS_ECAT_CORE1_LED_PIN_SCANNER=${RMCS_ECAT_CORE1_LED_PIN_SCANNER}
    -DRMCS_ECAT_CORE1_LED_CONFIRM=${RMCS_ECAT_CORE1_LED_CONFIRM}
)
add_rmcs_ecat_subbuild(
    rmcs_ecat_core0
    "${CMAKE_CURRENT_SOURCE_DIR}/core0"
    -DRMCS_ECAT_CORE1_CAN_PIN_SCANNER=${RMCS_ECAT_CORE1_CAN_PIN_SCANNER}
    -DRMCS_ECAT_CORE1_LED_PIN_SCANNER=${RMCS_ECAT_CORE1_LED_PIN_SCANNER}
    -DRMCS_ECAT_CORE1_LED_CONFIRM=${RMCS_ECAT_CORE1_LED_CONFIRM}
)
# core0 compiles the sec_core_img.c that the core1 build emits.
add_dependencies(rmcs_ecat_core0 rmcs_ecat_core1)

add_rmcs_ecat_subbuild(
    rmcs_ecat_bootloader
    "${CMAKE_CURRENT_SOURCE_DIR}/../bootloader"
    -DLIBRMCS_RMCS_BOARD_SUPERBUILDING=ON
    -DBOARD_BOOTLOADER_MODE=${BOARD_BOOTLOADER_MODE}
)

set(RMCS_ECAT_CORE0_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/rmcs_ecat_core0/compile_commands.json")
set(RMCS_ECAT_CORE1_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/rmcs_ecat_core1/compile_commands.json")
set(RMCS_ECAT_BOOTLOADER_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/rmcs_ecat_bootloader/compile_commands.json")
set(RMCS_ECAT_MERGED_COMPILE_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json")

add_custom_command(
    OUTPUT "${RMCS_ECAT_MERGED_COMPILE_COMMANDS}"
    COMMAND
    ${CMAKE_COMMAND}
    "-DOUTPUT_FILE=${RMCS_ECAT_MERGED_COMPILE_COMMANDS}"
    "-DINPUT_FILES=${RMCS_ECAT_CORE0_COMPILE_COMMANDS};${RMCS_ECAT_CORE1_COMPILE_COMMANDS};${RMCS_ECAT_BOOTLOADER_COMPILE_COMMANDS}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/merge_compile_commands.cmake"
    DEPENDS
    rmcs_ecat_core0
    rmcs_ecat_core1
    rmcs_ecat_bootloader
    "${RMCS_ECAT_CORE0_COMPILE_COMMANDS}"
    "${RMCS_ECAT_CORE1_COMPILE_COMMANDS}"
    "${RMCS_ECAT_BOOTLOADER_COMPILE_COMMANDS}"
    COMMENT "Merging rmcs_ecat compile commands"
    VERBATIM
)

add_custom_target(
    rmcs_ecat_compile_commands
    DEPENDS "${RMCS_ECAT_MERGED_COMPILE_COMMANDS}"
)

add_custom_target(rmcs_ecat ALL DEPENDS rmcs_ecat_compile_commands)
