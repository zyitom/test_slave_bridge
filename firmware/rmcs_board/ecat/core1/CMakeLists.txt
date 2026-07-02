# Core1 (fieldbus core) image of the RMCS EtherCAT stream bridge.
#
# Built as a secondary-core image: the SDK links it against ram_core1.ld and
# emits the binary as a C array into ../core0/src/sec_core_img.c, which the
# core0 project compiles in and loads into core1's ILM at boot (same pattern
# as samples/multicore/hello). Build this project BEFORE core0.

cmake_minimum_required(VERSION 3.13)

set(HPM_BUILD_TYPE "sec_core_img")
set(SEC_CORE_IMG_C_ARRAY_OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/../core0/src/sec_core_img.c)

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

project(rmcs_ecat_core1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

sdk_app_inc(../common)
sdk_app_src(src/main.cpp)

set_property(TARGET app PROPERTY CXX_STANDARD 23)
set_property(TARGET app PROPERTY CXX_STANDARD_REQUIRED ON)

generate_ide_projects()
