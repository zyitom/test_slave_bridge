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

add_executable(can_error_test can_error_test.cpp)
target_link_libraries(can_error_test PRIVATE librmcs-sdk)

add_executable(can_loopback_latency can_loopback_latency.cpp)
target_link_libraries(can_loopback_latency PRIVATE librmcs-sdk)

# CAN-FD loopback stress over the HPM6E8Y USB transport (CAN0->CAN1, CAN2->CAN3).
# The USB-transport twin of ecat_canfd_stress; no EtherCAT backend required.
add_executable(usb_canfd_stress usb_canfd_stress.cpp)
target_link_libraries(usb_canfd_stress PRIVATE librmcs-sdk)

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
endif()
