add_executable(test_compositor_workspace   test_workspace.cpp)
add_executable(test_compositor_tiling      test_tiling.cpp)
add_executable(test_compositor_ipc         test_ipc_compositor.cpp)

foreach(t test_compositor_workspace test_compositor_tiling test_compositor_ipc)
    target_link_libraries(${t} PRIVATE
        straylight-common
        GTest::gtest_main
        GTest::gmock
    )
    gtest_discover_tests(${t})
endforeach()
