set(BACKUP_SOURCES
    main.cpp
    engine.cpp
    scheduler.cpp
    ui.cpp
)

add_executable(straylight-backup ${BACKUP_SOURCES})

target_include_directories(straylight-backup PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL     REQUIRED egl)
pkg_check_modules(GLES    REQUIRED glesv2)

find_program(WAYLAND_SCANNER wayland-scanner)
set(XDG_SHELL_PROTOCOL "${CMAKE_SOURCE_DIR}/protocols/xdg-shell.xml")

if(WAYLAND_SCANNER AND EXISTS "${XDG_SHELL_PROTOCOL}")
    set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
    set(XDG_SHELL_CODE   "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")
    add_custom_command(
        OUTPUT ${XDG_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_HEADER}
        DEPENDS ${XDG_SHELL_PROTOCOL}
        COMMENT "Generating xdg-shell client header")
    add_custom_command(
        OUTPUT ${XDG_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_CODE}
        DEPENDS ${XDG_SHELL_PROTOCOL}
        COMMENT "Generating xdg-shell protocol code")
    target_sources(straylight-backup PRIVATE ${XDG_SHELL_HEADER} ${XDG_SHELL_CODE})
    target_include_directories(straylight-backup PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

target_link_libraries(straylight-backup PRIVATE
    straylight-common
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    imgui
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_compile_features(straylight-backup PRIVATE cxx_std_20)

install(TARGETS straylight-backup
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
