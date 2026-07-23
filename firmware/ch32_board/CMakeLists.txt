cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(librmcs-firmware LANGUAGES C CXX ASM)

include("${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/librmcs_firmware.cmake")
librmcs_setup_project_context(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

if(LIBRMCS_SOURCE_PATH_MAP_COMPILE_OPTIONS)
    add_compile_options(${LIBRMCS_SOURCE_PATH_MAP_COMPILE_OPTIONS})
endif()

set(CH32_BSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bsp")
set(CH32_WCH_ROOT "${CH32_BSP_ROOT}/wch")

# CH32H417 target selection: Qingke V5F main core, USB stack on the SS controller.
set(CH32_ARCH_FLAGS
    -march=rv32imafc_zicsr_zifencei
    -mabi=ilp32f
    -mcmodel=medany
)

set(CH32_LINKER_SCRIPT "${CH32_WCH_ROOT}/Ld/V5F/Link_v5f.ld")

# ---- WCH standard peripheral library (vendored, treated as third-party) ------
file(GLOB CH32_WCH_PERIPH_SOURCES CONFIGURE_DEPENDS
    "${CH32_WCH_ROOT}/Peripheral/src/*.c"
)

set(CH32_WCH_SOURCES
    ${CH32_WCH_PERIPH_SOURCES}
    "${CH32_WCH_ROOT}/Core/core_riscv.c"
    "${CH32_WCH_ROOT}/Debug/debug.c"
    "${CH32_WCH_ROOT}/Startup/startup_ch32h417_v5f.S"
)

set(CH32_WCH_INCLUDE_DIRS
    "${CH32_WCH_ROOT}/Core"
    "${CH32_WCH_ROOT}/Peripheral/inc"
    "${CH32_WCH_ROOT}/Debug"
)

# ---- CH372 USBSS device stack (vendored) -------------------------------------
file(GLOB CH32_USB_SOURCES CONFIGURE_DEPENDS "${CH32_BSP_ROOT}/usb/*.c")

# ---- Application -------------------------------------------------------------
# The librmcs C++ app (app/src) provides main() via app.cpp, replacing the WCH
# demo's app/User/main.c (which is intentionally excluded). The shared core
# protocol sources are compiled in alongside, as on the other boards.
file(GLOB_RECURSE CH32_APP_CXX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/src/*.cpp"
    "${LIBRMCS_PROJECT_ROOT}/core/src/*.cpp"
)

set(CH32_APP_SOURCES
    ${CH32_APP_CXX_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/app/User/system_ch32h417.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/User/ch32h417_it.c"
    "${CH32_BSP_ROOT}/syscalls.c"
)

set(CH32_APP_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/User"
    "${CH32_BSP_ROOT}/usb"
)

add_executable(ch32_board_app
    ${CH32_WCH_SOURCES}
    ${CH32_USB_SOURCES}
    ${CH32_APP_SOURCES}
)

target_include_directories(ch32_board_app PRIVATE
    ${CH32_WCH_INCLUDE_DIRS}
    ${CH32_APP_INCLUDE_DIRS}
    "${LIBRMCS_PROJECT_ROOT}/core/include"
    "${LIBRMCS_PROJECT_ROOT}"
)

target_compile_definitions(ch32_board_app PRIVATE
    Core_V5F
    Run_Core=Run_Core_V5F
    LIBRMCS_PROJECT_VERSION_STRING="${LIBRMCS_PROJECT_VERSION}"
    $<$<CONFIG:Release>:NDEBUG>
)

target_compile_options(ch32_board_app PRIVATE
    ${CH32_ARCH_FLAGS}
    -ffunction-sections
    -fdata-sections
    # WCH vendor headers use the GNU 'asm' keyword; the repo builds strict C11
    # (no GNU extensions), so alias it to the standard-mode spelling instead of
    # enabling gnu11 for the whole target.
    $<$<COMPILE_LANGUAGE:C>:-Dasm=__asm__>
    $<$<COMPILE_LANGUAGE:C>:-Dtypeof=__typeof__>
    $<$<COMPILE_LANGUAGE:CXX>:-Dasm=__asm__>
    $<$<COMPILE_LANGUAGE:CXX>:-Dtypeof=__typeof__>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
)

target_link_options(ch32_board_app PRIVATE
    ${CH32_ARCH_FLAGS}
    -nostartfiles
    --specs=nano.specs
    -T${CH32_LINKER_SCRIPT}
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    "-Wl,-Map=$<TARGET_FILE_DIR:ch32_board_app>/$<TARGET_FILE_BASE_NAME:ch32_board_app>.map"
)

set_target_properties(ch32_board_app PROPERTIES OUTPUT_NAME "ch32_board_app" SUFFIX ".elf")

# Produce a raw binary alongside the ELF for flashing.
add_custom_command(TARGET ch32_board_app POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        "$<TARGET_FILE:ch32_board_app>"
        "$<TARGET_FILE_DIR:ch32_board_app>/ch32_board_app.bin"
    COMMAND ${CMAKE_SIZE} "$<TARGET_FILE:ch32_board_app>"
    VERBATIM
)
