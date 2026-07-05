add_executable(test_ml_tensor test_tensor.cpp)
target_link_libraries(test_ml_tensor PRIVATE straylight-ml GTest::gtest_main)
gtest_discover_tests(test_ml_tensor)

add_executable(test_ml_graph test_graph.cpp)
target_link_libraries(test_ml_graph PRIVATE straylight-ml GTest::gtest_main)
gtest_discover_tests(test_ml_graph)

add_executable(test_ml_kv_cache test_kv_cache.cpp)
target_link_libraries(test_ml_kv_cache PRIVATE straylight-ml GTest::gtest_main)
gtest_discover_tests(test_ml_kv_cache)
