# Core0 (EtherCAT core) image of the RMCS EtherCAT stream bridge.
#
# Owns the integrated ESC and the Beckhoff SSC stack, bridges the process
# data stream chunks to the fieldbus core through the SHARE_RAM rings, and
# loads/releases the core1 image at boot. Build ../core1 FIRST -- it emits
# src/sec_core_img.c consumed here (see ../README.md).

cmake_minimum_required(VERSION 3.13)

if("${HPM_BUILD_TYPE}" STREQUAL "")
    set(HPM_BUILD_TYPE flash_xip)
endif()

# Enables the SDK flash-EEPROM-emulation component (must precede find_package).
set(CONFIG_EEPROM_EMULATION 1)

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

project(rmcs_ecat_core0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Beckhoff SSC sources are license-gated (ETG membership) and are NOT
# vendored in this repository. Generate them with the SSC Tool and place them
# under SSC/Src; see ../README.md for the exact steps.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SSC/Src/ecatslv.c")
    message(FATAL_ERROR
        "Beckhoff SSC sources not found at ecat/core0/SSC/Src.\n"
        "Generate them with the Beckhoff SSC Tool (ETG membership required), apply the\n"
        "SDK patch, and re-run CMake. Exact steps: firmware/rmcs_board/ecat/README.md.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sec_core_img.c")
    message(FATAL_ERROR
        "src/sec_core_img.c missing: build ../core1 first (it generates the core1 image\n"
        "array consumed by this project). Steps: firmware/rmcs_board/ecat/README.md.")
endif()

# Beckhoff SSC (generated).
sdk_inc(SSC/Src)
sdk_src(SSC/Src/coeappl.c)
sdk_src(SSC/Src/ecatappl.c)
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

sdk_app_inc(src ../common ${HPM_SDK_BASE}/samples/multicore/common)
sdk_app_src(${HPM_SDK_BASE}/samples/multicore/common/multicore_common.c)
sdk_app_src(src/ecat_main.c)
sdk_app_src(src/ecat_appl.c)
sdk_app_src(src/pd_glue.cpp)
sdk_app_src(src/sec_core_img.c)

set_property(TARGET app PROPERTY CXX_STANDARD 23)
set_property(TARGET app PROPERTY CXX_STANDARD_REQUIRED ON)

generate_ide_projects()
