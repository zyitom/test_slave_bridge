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
# Native CAN mailbox variant (see ../CMakeLists.txt). Uses the same driver +
# core sources as the protocol build; only main.cpp / can.* switch on the flag.
option(RMCS_ECAT_NATIVE_CAN "Build the native CAN mailbox variant" OFF)
# Hybrid fixed-PDO variant (see ../CMakeLists.txt). Reuses the protocol build's
# sources plus src/hybrid_link.cpp; main.cpp / can.* switch on the flag.
option(RMCS_ECAT_HYBRID_PD "Build the hybrid fixed-PDO variant" OFF)

if(RMCS_ECAT_NATIVE_CAN AND RMCS_ECAT_HYBRID_PD)
    message(FATAL_ERROR "RMCS_ECAT_NATIVE_CAN and RMCS_ECAT_HYBRID_PD are mutually exclusive.")
endif()
if(RMCS_ECAT_CORE1_LOOPBACK AND (RMCS_ECAT_NATIVE_CAN OR RMCS_ECAT_HYBRID_PD))
    message(FATAL_ERROR
        "RMCS_ECAT_CORE1_LOOPBACK cannot be combined with native or hybrid process data.")
endif()

# Component/board selection must precede find_package. The loopback bring-up
# image carries no peripheral code, so it must not pull in the board
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

if(RMCS_ECAT_CORE1_LOOPBACK)
    sdk_compile_definitions(-DRMCS_ECAT_CORE1_LOOPBACK=1)
else()
    # Native variant reuses all of the protocol build's sources (drivers + core
    # + host_link, which stays compiled but inert); only the RMCS_ECAT_NATIVE_CAN
    # define flips main.cpp / can.* onto the mailbox path.
    if(RMCS_ECAT_NATIVE_CAN)
        sdk_compile_definitions(-DRMCS_ECAT_NATIVE_CAN=1)
    endif()
    # Hybrid variant reuses the whole protocol build (the stream stays alive) and
    # adds src/hybrid_link.cpp for the cyclic CAN path. The stream ring is a raw
    # byte stream on core1; its PDO chunking is owned by core0 and the host.
    if(RMCS_ECAT_HYBRID_PD)
        sdk_compile_definitions(-DRMCS_ECAT_HYBRID_PD=1)
        sdk_app_src(src/hybrid_link.cpp)
    endif()
    # Protocol application: the shared app-layer drivers plus the EtherCAT
    # host link (transmission over the SHARE_RAM rings; session + dispatch
    # come from the shared link::HostSession).
    sdk_app_src(src/host_link.cpp)
    include("${LIBRMCS_PROJECT_ROOT}/firmware/rmcs_board/cmake/app_driver_sources.cmake")
    sdk_app_src(${LIBRMCS_APP_DRIVER_SOURCES})
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
