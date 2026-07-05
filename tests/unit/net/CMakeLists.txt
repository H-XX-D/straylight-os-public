add_executable(test_net_socket test_socket.cpp)
target_link_libraries(test_net_socket PRIVATE straylight-net GTest::gtest_main)
gtest_discover_tests(test_net_socket)

add_executable(test_net_transport test_transport.cpp)
target_link_libraries(test_net_transport PRIVATE straylight-net GTest::gtest_main)
gtest_discover_tests(test_net_transport)
