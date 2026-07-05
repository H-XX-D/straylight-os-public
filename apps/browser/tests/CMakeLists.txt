find_package(GTest REQUIRED)

add_executable(browser_test browser_test.cpp
    ../downloads.cpp)

target_include_directories(browser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/lib/common/include
)

target_link_libraries(browser_test PRIVATE
    straylight-common
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

target_compile_features(browser_test PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(browser_test)
