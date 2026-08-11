add_executable(mc02_app)

set(MC02_APP_TINYUSB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/tinyusb")
cmake_path(ABSOLUTE_PATH MC02_APP_TINYUSB_ROOT NORMALIZE)
set(MC02_APP_TINYUSB_COMMON_SOURCES
    "${MC02_APP_TINYUSB_ROOT}/src/tusb.c"
    "${MC02_APP_TINYUSB_ROOT}/src/common/tusb_fifo.c"
    "${MC02_APP_TINYUSB_ROOT}/src/device/usbd.c"
    "${MC02_APP_TINYUSB_ROOT}/src/portable/synopsys/dwc2/dwc2_common.c"
    "${MC02_APP_TINYUSB_ROOT}/src/portable/synopsys/dwc2/dcd_dwc2.c"
)

add_library(tinyusb_app OBJECT)
target_sources(tinyusb_app PRIVATE
    ${MC02_APP_TINYUSB_COMMON_SOURCES}
    "${MC02_APP_TINYUSB_ROOT}/src/class/vendor/vendor_device.c"
    "${MC02_APP_TINYUSB_ROOT}/src/class/dfu/dfu_rt_device.c"
)
target_include_directories(tinyusb_app SYSTEM PUBLIC
    "${MC02_APP_TINYUSB_ROOT}/src"
)
target_include_directories(tinyusb_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(tinyusb_app PUBLIC
    CFG_TUSB_MCU=OPT_MCU_STM32H7
)
target_link_libraries(tinyusb_app PUBLIC
    stm32cubemx
)

# stm32cubemx OBJECT sources (main.c, stm32h7xx_it.c) need TinyUSB and SEGGER headers
target_include_directories(stm32cubemx SYSTEM PRIVATE
    "${MC02_APP_TINYUSB_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(stm32cubemx PRIVATE
    CFG_TUSB_MCU=OPT_MCU_STM32H7
)
target_include_directories(stm32cubemx SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/SEGGER/RTT"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/SEGGER/Config"
    "${LIBRMCS_PROJECT_ROOT}/firmware/c_board"
)

file(GLOB_RECURSE MC02_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${LIBRMCS_PROJECT_ROOT}/core/src/*.cpp"
    "${LIBRMCS_PROJECT_ROOT}/core/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/SEGGER/RTT/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/SEGGER/RTT/*.S"
)
target_sources(mc02_app PRIVATE
    ${MC02_APP_SOURCES}
)

target_include_directories(mc02_app SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/SEGGER/RTT"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../c_board/bsp/SEGGER/Config"
)

target_link_libraries(mc02_app PRIVATE
    tinyusb_app
)

option(LIBRMCS_APP_IMU_ENABLE "Enable BMI088 initialization and runtime sampling" ON)
if(LIBRMCS_APP_IMU_ENABLE)
    target_compile_definitions(mc02_app PRIVATE LIBRMCS_APP_IMU_ENABLE=1)
endif()

option(LIBRMCS_APP_USB_DWC2_DMA "Use the DWC2 device controller internal DMA" OFF)
if(LIBRMCS_APP_USB_DWC2_DMA)
    target_compile_definitions(tinyusb_app PUBLIC LIBRMCS_APP_USB_DWC2_DMA=1)
endif()

# Same switch and same default as rmcs_board's: OFF, because it adds counters to
# the CAN forwarding hot path and occupies DataId::kUart0 on the uplink.
# Main-loop section timing on the kUart0 uplink, using the DWT cycle counter.
# OFF by default: it costs about one counter read per section boundary, and it
# shares DataId::kUart0 with LIBRMCS_APP_CAN_DIAG, so the two are mutually
# exclusive.
# RS-485 port on USART2 (hardware Driver Enable on PD4). OFF by default: the
# board does not currently have a transceiver in use, and the port occupies
# DataId::kUart0 -- the same slot the two diagnostic channels below emit on.
# Needs the USART2 DMA entries in mc02_slave.ioc to have been regenerated.
option(LIBRMCS_APP_RS485_ENABLE "Expose the USART2 RS-485 port as UART0" OFF)
if(LIBRMCS_APP_RS485_ENABLE)
    target_compile_definitions(mc02_app PRIVATE LIBRMCS_APP_RS485_ENABLE=1)
endif()

option(LIBRMCS_APP_LOOP_PROFILE "Emit main-loop section timing on the kUart0 uplink" OFF)
if(LIBRMCS_APP_LOOP_PROFILE)
    target_compile_definitions(mc02_app PRIVATE LIBRMCS_APP_LOOP_PROFILE=1)
endif()

option(LIBRMCS_APP_CAN_DIAG "Emit CAN telemetry on the kUart0 uplink" OFF)
if(LIBRMCS_APP_CAN_DIAG)
    target_compile_definitions(mc02_app PRIVATE LIBRMCS_APP_CAN_DIAG=1)
endif()

mc02_apply_target_options(
    mc02_app
    "${CMAKE_CURRENT_SOURCE_DIR}/../bsp/linker/STM32H723VGTx_APP.ld"
)

librmcs_add_dfu_image(
    TARGET mc02_app
    INPUT_ELF $<TARGET_FILE:mc02_app>
    OUTPUT_BINARY $<TARGET_FILE_DIR:mc02_app>/mc02_app.bin
    OUTPUT_DFU $<TARGET_FILE_DIR:mc02_app>/mc02_app.dfu
    VENDOR_ID 0xA11C
    PRODUCT_ID 0xD402
    DEVICE_ID 0x0300
    COMMENT "Generating mc02_app.dfu"
)
