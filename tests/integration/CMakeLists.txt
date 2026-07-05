# Integration tests for daemon startup ordering and dependency validation

add_executable(test_daemon_startup test_daemon_startup.cpp)
target_link_libraries(test_daemon_startup PRIVATE GTest::gtest_main)
target_include_directories(test_daemon_startup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pass the service directory path so tests can find .service files
target_compile_definitions(test_daemon_startup PRIVATE
    SERVICE_DIR="${CMAKE_SOURCE_DIR}/etc/systemd/system")
gtest_discover_tests(test_daemon_startup)
