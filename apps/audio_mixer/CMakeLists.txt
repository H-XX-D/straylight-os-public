set(MIXER_SOURCES
    main.cpp
    pipewire_client.cpp
    mixer.cpp
)

add_executable(straylight-audio-mixer ${MIXER_SOURCES})

target_include_directories(straylight-audio-mixer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND  REQUIRED wayland-client wayland-egl)
pkg_check_modules(EGL      REQUIRED egl)
pkg_check_modules(GLES     REQUIRED glesv2)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

find_program(WAYLAND_SCANNER wayland-scanner)
set(XDG_SHELL_PROTOCOL "${CMAKE_SOURCE_DIR}/protocols/xdg-shell.xml")

if(WAYLAND_SCANNER AND EXISTS "${XDG_SHELL_PROTOCOL}")
    set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
    set(XDG_SHELL_CODE   "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")
    add_custom_command(
        OUTPUT ${XDG_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_HEADER}
        DEPENDS ${XDG_SHELL_PROTOCOL})
    add_custom_command(
        OUTPUT ${XDG_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_PROTOCOL} ${XDG_SHELL_CODE}
        DEPENDS ${XDG_SHELL_PROTOCOL})
    target_sources(straylight-audio-mixer PRIVATE ${XDG_SHELL_HEADER} ${XDG_SHELL_CODE})
    target_include_directories(straylight-audio-mixer PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

target_include_directories(straylight-audio-mixer SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(straylight-audio-mixer PRIVATE
    straylight-common
    ${WAYLAND_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    imgui
)

target_compile_options(straylight-audio-mixer PRIVATE
    ${PIPEWIRE_CFLAGS_OTHER}
)

target_compile_features(straylight-audio-mixer PRIVATE cxx_std_20)

install(TARGETS straylight-audio-mixer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
