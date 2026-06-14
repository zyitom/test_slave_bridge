cmake_minimum_required(VERSION 3.22)
enable_language(C ASM)

set(MC02_BSP_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MX_HAL_DRV      "${MC02_BSP_ROOT}/stm32h7xx-hal-driver")
set(MX_CUBEMX       "${MC02_BSP_ROOT}/cubemx")
set(MX_CMSIS_DEVICE "${MC02_BSP_ROOT}/cmsis-device-h7")
set(MX_CMSIS_CORE   "${MC02_BSP_ROOT}/../../c_board/bsp/cmsis-core")

set(MX_Defines_Syms
    USE_HAL_DRIVER
    STM32H723xx
    $<$<CONFIG:Debug>:DEBUG>
)

set(MX_Include_Dirs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MX_CUBEMX}/Core/Inc
    ${MX_HAL_DRV}/Inc
    ${MX_HAL_DRV}/Inc/Legacy
    ${MX_CMSIS_DEVICE}/Include
    ${MX_CMSIS_CORE}/Core/Include
)

set(MX_Application_Src
    ${MX_CUBEMX}/Core/Src/main.c
    ${MX_CUBEMX}/Core/Src/gpio.c
    ${MX_CUBEMX}/Core/Src/fdcan.c
    ${MX_CUBEMX}/Core/Src/dma.c
    ${MX_CUBEMX}/Core/Src/bdma.c
    ${MX_CUBEMX}/Core/Src/spi.c
    ${MX_CUBEMX}/Core/Src/tim.c
    ${MX_CUBEMX}/Core/Src/usart.c
    ${MX_CUBEMX}/Core/Src/memorymap.c
    ${MX_CUBEMX}/Core/Src/stm32h7xx_it.c
    ${MX_CUBEMX}/Core/Src/stm32h7xx_hal_msp.c
    ${MX_CUBEMX}/Core/Src/sysmem.c
    ${MX_CUBEMX}/Core/Src/syscalls.c
    ${MX_CUBEMX}/Core/Startup/startup_stm32h723vgtx.s
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_device.c
)

set(STM32_Drivers_Src
    ${MX_CUBEMX}/Core/Src/system_stm32h7xx.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_fdcan.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_rcc.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_rcc_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_flash.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_flash_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_gpio.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_dma.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_dma_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_mdma.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_pwr.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_pwr_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_cortex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_exti.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_spi.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_spi_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_tim.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_tim_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_uart.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_uart_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_pcd.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_pcd_ex.c
    ${MX_HAL_DRV}/Src/stm32h7xx_ll_usb.c
    ${MX_HAL_DRV}/Src/stm32h7xx_hal_hsem.c
)

add_library(stm32cubemx OBJECT)
target_sources(stm32cubemx PRIVATE ${STM32_Drivers_Src})
target_sources(stm32cubemx PRIVATE ${MX_Application_Src})
target_include_directories(stm32cubemx PUBLIC ${MX_Include_Dirs})
target_compile_definitions(stm32cubemx PUBLIC ${MX_Defines_Syms})
