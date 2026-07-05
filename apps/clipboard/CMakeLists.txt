set(CLIPBOARD_SOURCES
    main.cpp
    history.cpp
    watcher.cpp
)

add_executable(straylight-clipboard ${CLIPBOARD_SOURCES})

target_include_directories(straylight-clipboard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL     REQUIRED egl)
pkg_check_modules(GLES    REQUIRED glesv2)

find_program(WAYLAND_SCANNER wayland-scanner)
set(XDG_SHELL_PROTOCOL   "${CMAKE_SOURCE_DIR}/protocols/xdg-shell.xml")
set(WLR_LAYER_PROTOCOL   "${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml")
set(DATA_CTRL_PROTOCOL   "${CMAKE_SOURCE_DIR}/protocols/wlr-data-control-unstable-v1.xml")

if(WAYLAND_SCANNER AND EXISTS "${XDG_SHELL_PROTOCOL}")
    set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
    set(XDG_SHELL_CODE   "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")
    add_custom_command(
        OUTPUT ${XDG_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_HEADER}
        DEPENDS ${XDG_SHELL_PROTOCOL}
        COMMENT "xdg-shell header")
    add_custom_command(
        OUTPUT ${XDG_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_CODE}
        DEPENDS ${XDG_SHELL_PROTOCOL}
        COMMENT "xdg-shell code")
    target_sources(straylight-clipboard PRIVATE ${XDG_SHELL_HEADER} ${XDG_SHELL_CODE})
    target_include_directories(straylight-clipboard PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(WAYLAND_SCANNER AND EXISTS "${DATA_CTRL_PROTOCOL}")
    set(DATA_CTRL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/wlr-data-control-unstable-v1-client-protocol.h")
    set(DATA_CTRL_CODE   "${CMAKE_CURRENT_BINARY_DIR}/wlr-data-control-unstable-v1-protocol.c")
    add_custom_command(
        OUTPUT ${DATA_CTRL_HEADER}
        COMMAND ${WAYLAND_SCANNER} client-header ${DATA_CTRL_PROTOCOL} ${DATA_CTRL_HEADER}
        DEPENDS ${DATA_CTRL_PROTOCOL}
        COMMENT "wlr-data-control header")
    add_custom_command(
        OUTPUT ${DATA_CTRL_CODE}
        COMMAND ${WAYLAND_SCANNER} private-code ${DATA_CTRL_PROTOCOL} ${DATA_CTRL_CODE}
        DEPENDS ${DATA_CTRL_PROTOCOL}
        COMMENT "wlr-data-control code")
    target_sources(straylight-clipboard PRIVATE ${DATA_CTRL_HEADER} ${DATA_CTRL_CODE})
    target_include_directories(straylight-clipboard PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(straylight-clipboard PRIVATE HAVE_DATA_CONTROL_PROTOCOL=1)
endif()

target_link_libraries(straylight-clipboard PRIVATE
    straylight-common
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    imgui
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_compile_features(straylight-clipboard PRIVATE cxx_std_20)

install(TARGETS straylight-clipboard
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
