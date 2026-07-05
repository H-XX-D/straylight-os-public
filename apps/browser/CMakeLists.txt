set(BROWSER_SOURCES
    main.cpp
    engine.cpp
    tab_manager.cpp
    downloads.cpp
)

add_executable(straylight-browser ${BROWSER_SOURCES})

find_package(PkgConfig REQUIRED)

# WebKitGTK — try 6.0 (GTK4) then fall back to 4.1 (GTK3)
pkg_check_modules(WEBKIT webkit2gtk-6.0)
if(NOT WEBKIT_FOUND)
    pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.1)
endif()

pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL     REQUIRED egl)
pkg_check_modules(GLES    REQUIRED glesv2)
pkg_check_modules(GTK     REQUIRED gtk4)

# Generate xdg-shell protocol bindings (reuse from terminal if already built)
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
    target_sources(straylight-browser PRIVATE
        ${XDG_SHELL_HEADER}
        ${XDG_SHELL_CODE}
    )
    target_include_directories(straylight-browser PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
    )
endif()

target_include_directories(straylight-browser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
    ${WEBKIT_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
)

target_link_libraries(straylight-browser PRIVATE
    straylight-common
    imgui
    ${WEBKIT_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    ${GTK_LIBRARIES}
    nlohmann_json::nlohmann_json
)

target_compile_options(straylight-browser PRIVATE
    ${WEBKIT_CFLAGS_OTHER}
    ${GTK_CFLAGS_OTHER}
)

target_compile_features(straylight-browser PRIVATE cxx_std_20)

install(TARGETS straylight-browser
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---- Tests ----
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
