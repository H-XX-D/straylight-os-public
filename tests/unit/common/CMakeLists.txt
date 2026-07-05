add_executable(test_common_result test_result.cpp)
target_link_libraries(test_common_result PRIVATE straylight-common GTest::gtest_main)
gtest_discover_tests(test_common_result)

add_executable(test_common_log test_log.cpp)
target_link_libraries(test_common_log PRIVATE straylight-common GTest::gtest_main)
gtest_discover_tests(test_common_log)

add_executable(test_common_config test_config.cpp)
target_link_libraries(test_common_config PRIVATE straylight-common GTest::gtest_main)
gtest_discover_tests(test_common_config)

add_executable(test_common_ipc test_ipc.cpp)
target_link_libraries(test_common_ipc PRIVATE straylight-common GTest::gtest_main)
gtest_discover_tests(test_common_ipc)

add_executable(test_error test_error.cpp)
target_link_libraries(test_error PRIVATE straylight-common GTest::gtest_main)
gtest_discover_tests(test_error)
