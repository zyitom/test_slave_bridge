add_executable(servo_test servo_test.cpp)
target_link_libraries(servo_test PRIVATE librmcs-sdk)

add_executable(hpm5321_test hpm5321_test.cpp)
target_link_libraries(hpm5321_test PRIVATE librmcs-sdk)

add_executable(dm_motor_stress dm_motor_stress.cpp)
target_link_libraries(dm_motor_stress PRIVATE librmcs-sdk)

add_executable(uart_stress uart_stress.cpp)
target_link_libraries(uart_stress PRIVATE librmcs-sdk)

add_executable(rx_monitor rx_monitor.cpp)
target_link_libraries(rx_monitor PRIVATE librmcs-sdk)

add_executable(motor_pid_sine motor_pid_sine.cpp)
target_link_libraries(motor_pid_sine PRIVATE librmcs-sdk)

# Two-board suite: a pair of HPM5321 dual-CAN-FD boards wired CAN0<->CAN0,
# CAN1<->CAN1 and UART0<->UART0. Unlike the single-board loopback tools, every
# frame crosses a real bus between two independent CAN controllers.
add_executable(dual_board_test dual_board_test.cpp)
target_link_libraries(dual_board_test PRIVATE librmcs-sdk)

# Heterogeneous pair (built for mc02 <-> HPM5321 dual-CAN-FD hardware): goes through the
# board-agnostic BoardSession, discovers the wiring, and separates USB packet
# rate from CAN frame rate -- the two ends run at different USB speeds.
add_executable(mixed_board_test mixed_board_test.cpp)
target_link_libraries(mixed_board_test PRIVATE librmcs-sdk)

add_executable(can_error_test can_error_test.cpp)
target_link_libraries(can_error_test PRIVATE librmcs-sdk)

add_executable(can_loopback_latency can_loopback_latency.cpp)
target_link_libraries(can_loopback_latency PRIVATE librmcs-sdk)

# Host-path-only USB probe: times a control transfer to EP0, so it needs no CAN
# wiring and no firmware cooperation. The instrument behind HOST_TUNING.md's
# host-side A/B numbers; not a substitute for can_loopback_latency's board RTT.
add_executable(usb_ep0_rtt usb_ep0_rtt.cpp)
target_link_libraries(usb_ep0_rtt PRIVATE librmcs-sdk)
target_include_directories(usb_ep0_rtt SYSTEM PRIVATE ${LIBUSB_INCLUDE_DIRS})

# Splits board-level one-way latency into fixed floor vs per-frame pipeline step,
# by sending a burst of CAN frames inside a single USB packet. Answers whether
# the firmware sits on the critical path or is hidden behind the CAN wire.
add_executable(can_path_split can_path_split.cpp)
target_link_libraries(can_path_split PRIVATE librmcs-sdk)

# USB bulk OUT packet-rate ceiling: sends each record in its own packet so the
# packet rate can be raised without raising the per-bus CAN frame rate. Separates
# the USB transaction limit from the CAN wire limit that caps dual_board_test.
add_executable(usb_packet_rate usb_packet_rate.cpp)
target_link_libraries(usb_packet_rate PRIVATE librmcs-sdk)

add_executable(can_frame_type_test can_frame_type_test.cpp)
target_link_libraries(can_frame_type_test PRIVATE librmcs-sdk)

# CAN-FD loopback stress over the HPM6E8Y USB transport (CAN0->CAN1, CAN2->CAN3).
# The USB-transport twin of ecat_canfd_stress; no EtherCAT backend required.
add_executable(usb_canfd_stress usb_canfd_stress.cpp)
target_link_libraries(usb_canfd_stress PRIVATE librmcs-sdk)

# Same flood as usb_canfd_stress, plus a decoder for the UART0 telemetry a
# LIBRMCS_CAN_DIAG firmware emits. Prints the MCAN and PLIC state around the
# moment forwarding stops (see firmware/rmcs_board/app/src/diag/can_diag.hpp).
add_executable(can_stall_probe can_stall_probe.cpp)
target_link_libraries(can_stall_probe PRIVATE librmcs-sdk)

# The main-loop-period half of the same telemetry, for the HPM5321. Separate
# from can_stall_probe because that tool is bound to the HPM6E8Y's four buses
# and its single-board CAN0->CAN1 / CAN2->CAN3 loopback, neither of which exists
# on a two-board HPM5321 rig. Reads only; drive the load from another process.
add_executable(hpm5321_loop_probe hpm5321_loop_probe.cpp)
target_link_libraries(hpm5321_loop_probe PRIVATE librmcs-sdk)

# Prints core1's diagnostic ring, relayed as UART0 uplink frames by a firmware
# built with LIBRMCS_DIAG_OVER_USB=ON. The board's console UART sits on an
# unpopulated debug header, so this is the only way to read that log here.
add_executable(core1_log core1_log.cpp)
target_link_libraries(core1_log PRIVATE librmcs-sdk)

if(LIBRMCS_ENABLE_SOEM OR LIBRMCS_ENABLE_IGH)
    add_executable(ecat_stream_latency ecat_stream_latency.cpp)
    target_link_libraries(ecat_stream_latency PRIVATE librmcs-sdk)
    # Raw-transport diagnostic: deliberately uses the internal transport API
    # (host/src/...), which needs the repository root on the include path.
    target_include_directories(
        ecat_stream_latency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    # Tell the tool which backends are linked in so it can offer them at
    # run time via RMCS_ECAT_BACKEND (see the source).
    if(LIBRMCS_ENABLE_SOEM)
        target_compile_definitions(ecat_stream_latency PRIVATE EXAMPLE_HAVE_SOEM=1)
    endif()
    if(LIBRMCS_ENABLE_IGH)
        target_compile_definitions(ecat_stream_latency PRIVATE EXAMPLE_HAVE_IGH=1)
    endif()
endif()

if(LIBRMCS_ENABLE_SOEM OR LIBRMCS_ENABLE_IGH)
    # Full-stack test; the EtherCAT backend is picked at run time via
    # RMCS_ECAT_BACKEND=soem|igh (handled inside protocol::Handler).
    add_executable(ecat_board_test ecat_board_test.cpp)
    target_link_libraries(ecat_board_test PRIVATE librmcs-sdk)

    # CAN-FD loopback stress over the EtherCAT bridge (CAN0->CAN1, CAN2->CAN3).
    add_executable(ecat_canfd_stress ecat_canfd_stress.cpp)
    target_link_libraries(ecat_canfd_stress PRIVATE librmcs-sdk)

    # Queue-free CAN0->CAN1 RTT comparison over USB or EtherCAT.
    add_executable(bridge_can_loopback_latency bridge_can_loopback_latency.cpp)
    target_link_libraries(bridge_can_loopback_latency PRIVATE librmcs-sdk)
endif()

add_executable(motor_load motor_load.cpp)
target_link_libraries(motor_load PRIVATE librmcs-sdk)

add_executable(imu_dbus_check imu_dbus_check.cpp)
target_link_libraries(imu_dbus_check PRIVATE librmcs-sdk)

add_executable(mc02_can_diag mc02_can_diag.cpp)
target_link_libraries(mc02_can_diag PRIVATE librmcs-sdk)

add_executable(mc02_tinyusb_probe mc02_tinyusb_probe.cpp)
target_link_libraries(mc02_tinyusb_probe PRIVATE librmcs-sdk)

add_executable(mc02_dual_can_probe mc02_dual_can_probe.cpp)
target_link_libraries(mc02_dual_can_probe PRIVATE librmcs-sdk)
