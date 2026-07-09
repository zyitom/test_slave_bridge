# Core1 (fieldbus core) image of the RMCS EtherCAT stream bridge.
#
# Built as a secondary-core image: the SDK links it against ram_core1.ld and
# emits the binary as a C array into ../core0/src/sec_core_img.c, which the
# core0 project compiles in and loads into core1's ILM at boot (same pattern
# as samples/multicore/hello). Build this project BEFORE core0 -- or use the
# ecat superbuild (../CMakeLists.txt), which orders everything.

cmake_minimum_required(VERSION 3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(HPM_BUILD_TYPE "sec_core_img")
set(SEC_CORE_IMG_C_ARRAY_OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/../core0/src/sec_core_img.c)

set(BOARD "hpm6e80ivm1" CACHE STRING "Target board")
set(BOARD_SEARCH_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../boards" CACHE PATH "Board search path")

# P1 bring-up image: lossless byte echo instead of the protocol application
# (pairs with the host tool ecat_stream_latency for byte-exact validation).
option(RMCS_ECAT_CORE1_LOOPBACK "Build the loopback bring-up image" OFF)
option(RMCS_ECAT_CORE1_CAN_PIN_SCANNER "Build the HPM6E8Y CAN pin scanner" OFF)
option(RMCS_ECAT_CORE1_LED_PIN_SCANNER "Build the HPM6E8Y LED GPIO pin scanner" OFF)
option(RMCS_ECAT_CORE1_LED_CONFIRM "Build the HPM6E8Y known-LED confirmation image" OFF)

# The alternate core1 images are mutually exclusive; at most one may be ON.
set(_rmcs_core1_modes 0)
foreach(_mode
        RMCS_ECAT_CORE1_LOOPBACK
        RMCS_ECAT_CORE1_CAN_PIN_SCANNER
        RMCS_ECAT_CORE1_LED_PIN_SCANNER
        RMCS_ECAT_CORE1_LED_CONFIRM)
    if(${_mode})
        math(EXPR _rmcs_core1_modes "${_rmcs_core1_modes} + 1")
    endif()
endforeach()
if(_rmcs_core1_modes GREATER 1)
    message(FATAL_ERROR "RMCS_ECAT_CORE1_LOOPBACK, RMCS_ECAT_CORE1_CAN_PIN_SCANNER, RMCS_ECAT_CORE1_LED_PIN_SCANNER and RMCS_ECAT_CORE1_LED_CONFIRM are mutually exclusive")
endif()

# Component/board selection must precede find_package. The loopback image is
# a pure ring echo with zero peripheral code, so it must not pull in the board
# application layer (ISR table) either.
if(NOT RMCS_ECAT_CORE1_LOOPBACK)
    set(CONFIG_DMA_MGR 1)
    set(LIBRMCS_BUILDING_ECAT_CORE1 1)
endif()

set(ENV{HPM_SDK_BASE} "${CMAKE_CURRENT_SOURCE_DIR}/../../bsp/hpm_sdk")
find_package(hpm-sdk REQUIRED HINTS "$ENV{HPM_SDK_BASE}")

project(rmcs_ecat_core1 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
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

if(CMAKE_BUILD_TYPE STREQUAL "debug")
    sdk_compile_options(-Og)
endif()

set(LIBRMCS_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
cmake_path(ABSOLUTE_PATH LIBRMCS_PROJECT_ROOT NORMALIZE)

sdk_app_inc(src ../common)
sdk_app_inc("${LIBRMCS_PROJECT_ROOT}/core/include")
sdk_app_inc("${LIBRMCS_PROJECT_ROOT}")

sdk_app_src(src/main.cpp)

if(RMCS_ECAT_CORE1_CAN_PIN_SCANNER)
    sdk_compile_definitions(-DRMCS_ECAT_CORE1_CAN_PIN_SCANNER=1)
elseif(RMCS_ECAT_CORE1_LED_PIN_SCANNER)
    sdk_compile_definitions(-DRMCS_ECAT_CORE1_LED_PIN_SCANNER=1)
elseif(RMCS_ECAT_CORE1_LED_CONFIRM)
    sdk_compile_definitions(-DRMCS_ECAT_CORE1_LED_CONFIRM=1)
elseif(RMCS_ECAT_CORE1_LOOPBACK)
    sdk_compile_definitions(-DRMCS_ECAT_CORE1_LOOPBACK=1)
else()
    # Protocol application: the shared app-layer drivers plus the EtherCAT
    # host link (session + dispatch over the SHARE_RAM rings).
    sdk_app_src(src/host_link.cpp)
    sdk_app_src(
        "${LIBRMCS_PROJECT_ROOT}/firmware/rmcs_board/app/src/can/can.cpp"
        "${LIBRMCS_PROJECT_ROOT}/firmware/rmcs_board/app/src/timer/timer.cpp"
        "${LIBRMCS_PROJECT_ROOT}/firmware/rmcs_board/app/src/uart/uart.cpp"
        "${LIBRMCS_PROJECT_ROOT}/firmware/rmcs_board/app/src/utility/assert.cpp"
    )
    file(GLOB_RECURSE LIBRMCS_CORE_SOURCE CONFIGURE_DEPENDS
        "${LIBRMCS_PROJECT_ROOT}/core/src/*.cpp"
        "${LIBRMCS_PROJECT_ROOT}/core/src/*.c"
    )
    sdk_app_src(${LIBRMCS_CORE_SOURCE})
endif()

set_property(TARGET app PROPERTY CXX_STANDARD 23)
set_property(TARGET app PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET app PROPERTY CXX_EXTENSIONS OFF)

get_target_property(_hpm_incs ${HPM_SDK_LIB_ITF} INTERFACE_INCLUDE_DIRECTORIES)
if(_hpm_incs)
    set_target_properties(${HPM_SDK_LIB_ITF} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_hpm_incs}")
endif()

generate_ide_projects()
