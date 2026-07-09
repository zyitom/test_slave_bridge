sdk_inc(.)
sdk_src(board.c)
sdk_compile_definitions_ifdef(RMCS_ECAT_CORE1_CAN_PIN_SCANNER RMCS_ECAT_CORE1_CAN_PIN_SCANNER=1)
sdk_compile_definitions_ifdef(RMCS_ECAT_CORE1_LED_PIN_SCANNER RMCS_ECAT_CORE1_LED_PIN_SCANNER=1)
sdk_compile_definitions_ifdef(RMCS_ECAT_CORE1_LED_CONFIRM RMCS_ECAT_CORE1_LED_CONFIRM=1)

# Secondary-core (core1) images see the same board files; mirror the SDK board
# convention so BOARD_RUNNING_CORE selects the per-core defaults (console off,
# mchtmr1, ...).
sdk_compile_definitions_ifdef(BUILD_FOR_SECONDARY_CORE BOARD_RUNNING_CORE=HPM_CORE1)

# Unique per board: the DFU bootloader enumerates with this PID and dfu-util
# matches images against it (0xA901 hpm5321, 0xA902 hpm5321_dual_can, 0xA903 hpm6e80ivm1).
set_property(GLOBAL PROPERTY LIBRMCS_USB_PID 0xA904)
sdk_compile_definitions(LIBRMCS_USB_PID=0xA904)

# The fieldbus application (EtherCAT bridge core1) reuses the shared app-layer
# drivers; this board directory provides their port tables and pin hooks.
if(LIBRMCS_BUILDING_ECAT_CORE1)
    sdk_app_inc(app)
    if(RMCS_ECAT_CORE1_CAN_PIN_SCANNER)
        sdk_app_src(app/can_pin_scanner.cpp)
    elseif(RMCS_ECAT_CORE1_LED_PIN_SCANNER)
        sdk_app_src(app/led_pin_scanner.cpp)
    elseif(RMCS_ECAT_CORE1_LED_CONFIRM)
        sdk_app_src(app/led_confirm.cpp)
    else()
        sdk_app_src(app/board_app.cpp)
    endif()
endif()
