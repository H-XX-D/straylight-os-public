add_executable(test_theme_engine test_theme_engine.cpp)
target_include_directories(test_theme_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/shell
    ${CMAKE_SOURCE_DIR}/shell/themes
    ${CMAKE_SOURCE_DIR}/lib/common/include
)
target_link_libraries(test_theme_engine PRIVATE
    GTest::gtest_main
    straylight-common
    imgui
)
gtest_discover_tests(test_theme_engine)

add_executable(test_top_bar test_top_bar.cpp)
target_include_directories(test_top_bar PRIVATE
    ${CMAKE_SOURCE_DIR}/shell
    ${CMAKE_SOURCE_DIR}/shell/panels
    ${CMAKE_SOURCE_DIR}/lib/common/include
)
target_link_libraries(test_top_bar PRIVATE
    GTest::gtest_main
    straylight-common
    imgui
)
gtest_discover_tests(test_top_bar)
