sdk_inc(.)
sdk_src(board.c)

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
    sdk_app_src(app/board_app.cpp)
endif()
