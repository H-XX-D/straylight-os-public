set(PLAYER_SOURCES
    main.cpp
    playback.cpp
    playlist.cpp
    visualizer.cpp
)

add_executable(straylight-player ${PLAYER_SOURCES})

target_include_directories(straylight-player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND  REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL      REQUIRED egl)
pkg_check_modules(GLES     REQUIRED glesv2)
pkg_check_modules(GST      REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP  REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_PBUTILS REQUIRED gstreamer-pbutils-1.0)

# Generate xdg-shell protocol bindings
find_program(WAYLAND_SCANNER wayland-scanner)
set(XDG_SHELL_PROTOCOL "${CMAKE_SOURCE_DIR}/protocols/xdg-shell.xml")

if(WAYLAND_SCANNER AND EXISTS "${XDG_SHELL_PROTOCOL}")
    set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
    set(XDG_SHELL_CODE   "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")

    add_custom_command(
        OUTPUT ${XDG_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} client-header
                ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_HEADER}
        DEPENDS ${XDG_SHELL_PROTOCOL}
    )
    add_custom_command(
        OUTPUT ${XDG_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} private-code
                ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_CODE}
        DEPENDS ${XDG_SHELL_PROTOCOL}
    )
    target_sources(straylight-player PRIVATE
        ${XDG_SHELL_HEADER}
        ${XDG_SHELL_CODE}
    )
    target_include_directories(straylight-player PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
    )
endif()

target_include_directories(straylight-player PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_PBUTILS_INCLUDE_DIRS}
)

target_link_libraries(straylight-player PRIVATE
    straylight-common
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_PBUTILS_LIBRARIES}
    imgui
)

target_compile_options(straylight-player PRIVATE
    ${GST_CFLAGS_OTHER}
)

target_compile_features(straylight-player PRIVATE cxx_std_20)

install(TARGETS straylight-player
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
