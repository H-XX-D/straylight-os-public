# Tests for greeter, oobe, and wizard apps

# PAM auth tests
add_executable(test_pam_auth test_pam_auth.cpp)
target_include_directories(test_pam_auth PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/greeter
    ${CMAKE_SOURCE_DIR}/lib/common/include
)
target_link_libraries(test_pam_auth PRIVATE
    GTest::gtest_main
    straylight-common
)
gtest_discover_tests(test_pam_auth)

# OOBE state tests
add_executable(test_oobe_state test_oobe_state.cpp)
target_include_directories(test_oobe_state PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/oobe
    ${CMAKE_SOURCE_DIR}/lib/common/include
)
target_link_libraries(test_oobe_state PRIVATE
    GTest::gtest_main
    straylight-common
)
gtest_discover_tests(test_oobe_state)

# Wizard state/firstboot tests
add_executable(test_wizard_state test_wizard_state.cpp)
target_include_directories(test_wizard_state PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/wizard
    ${CMAKE_SOURCE_DIR}/lib/common/include
)
target_link_libraries(test_wizard_state PRIVATE
    GTest::gtest_main
    straylight-common
)
gtest_discover_tests(test_wizard_state)

# Widget panel tests (registry, GPU HUD parsing, CPU topology parsing)
add_executable(test_widgets test_widgets.cpp)
target_include_directories(test_widgets PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/widgets
    ${CMAKE_SOURCE_DIR}/lib/common/include
)
target_link_libraries(test_widgets PRIVATE
    GTest::gtest_main
    straylight-widgets
    straylight-common
)
gtest_discover_tests(test_widgets)
