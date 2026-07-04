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

if(LIBRMCS_ENABLE_SOEM)
    add_executable(ecat_stream_latency ecat_stream_latency.cpp)
    target_link_libraries(ecat_stream_latency PRIVATE librmcs-sdk)
endif()
