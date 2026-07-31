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

# Both the standalone USB application and the EtherCAT fieldbus core reuse the
# shared app-layer drivers; this board directory provides their port tables and
# pin hooks.
if(LIBRMCS_BUILDING_FIRMWARE_APP OR LIBRMCS_BUILDING_ECAT_CORE1)
    sdk_app_inc(app)
    sdk_app_src(app/board_app.cpp)
endif()

# Two orthogonal knobs feed board_init_clock() (see the comment there for the
# clock-group semantics):
#
#   BOARD_FIELDBUS_ON_CORE0    CAN0..CAN3 + UART1 sit in core0's clock group
#   BOARD_SECONDARY_CORE_USED  core1 gets released, so group 1 must exist
#                              (cpu1 + mchtmr1, and the fieldbus peripherals
#                              only when they are not already in group 0)
#
# The standalone USB application is single-core: it runs USB and CAN on core0
# and never releases core1, so a fieldbus peripheral left in group 1 would stay
# clock-gated forever. Every other image on this board is dual-core.
#
# The three valid combinations:
#
#   image                        FIELDBUS_ON_CORE0  SECONDARY_CORE_USED
#   single-core USB app                  1                  0
#   core-swap layout (app + core1)       1                  1
#   current EtherCAT bridge              0                  1
#
# The middle row is selected automatically: the app project turns on
# LIBRMCS_APP_RELEASE_CORE1 when the superbuild is asked to build and release the
# EtherCAT core1 image (-DLIBRMCS_RELEASE_CORE1=ON), which is exactly the
# condition under which group 1 becomes meaningful. Either variable may also be
# pre-set by a parent project to force a combination.
if(NOT DEFINED BOARD_FIELDBUS_ON_CORE0)
    if(LIBRMCS_BUILDING_FIRMWARE_APP)
        set(BOARD_FIELDBUS_ON_CORE0 1)
    else()
        set(BOARD_FIELDBUS_ON_CORE0 0)
    endif()
endif()
if(NOT DEFINED BOARD_SECONDARY_CORE_USED)
    if(NOT LIBRMCS_BUILDING_FIRMWARE_APP OR LIBRMCS_APP_RELEASE_CORE1)
        set(BOARD_SECONDARY_CORE_USED 1)
    else()
        set(BOARD_SECONDARY_CORE_USED 0)
    endif()
endif()
sdk_compile_definitions(BOARD_FIELDBUS_ON_CORE0=${BOARD_FIELDBUS_ON_CORE0})
sdk_compile_definitions(BOARD_SECONDARY_CORE_USED=${BOARD_SECONDARY_CORE_USED})
