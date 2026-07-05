cmake_minimum_required(VERSION 3.20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND     REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL         REQUIRED egl)
pkg_check_modules(GLES        REQUIRED glesv2)
find_package(nlohmann_json    REQUIRED)

set(INSTALLER_SOURCES
    main.cpp
    disk_select.cpp
)

add_executable(straylight-installer ${INSTALLER_SOURCES})

target_include_directories(straylight-installer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
    ${CMAKE_BINARY_DIR}/protocols   # generated xdg-shell headers
)

target_link_libraries(straylight-installer PRIVATE
    straylight-common
    nlohmann_json::nlohmann_json
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    imgui
    xdg-shell-protocol  # generated from wayland-protocols
)

install(TARGETS straylight-installer
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/straylight
)
