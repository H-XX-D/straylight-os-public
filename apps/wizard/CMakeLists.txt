set(WIZARD_SOURCES
    main.cpp
    firstboot.cpp
    pages/theme_picker.cpp
    pages/layout_config.cpp
    pages/ide_setup.cpp
    pages/ml_setup.cpp
    pages/tour.cpp
    pages/summary.cpp
)

add_executable(straylight-wizard ${WIZARD_SOURCES})

target_include_directories(straylight-wizard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
    ${STRAYLIGHT_IMGUI_INCLUDE_DIR}/backends
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES REQUIRED glesv2)

target_link_libraries(straylight-wizard PRIVATE
    straylight-common
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    imgui
)

install(TARGETS straylight-wizard
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
