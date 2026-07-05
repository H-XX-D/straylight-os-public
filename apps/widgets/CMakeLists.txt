# apps/widgets/CMakeLists.txt
# ML/HPC/System/Research widgets — compiled as a static library linked into the shell

file(GLOB_RECURSE WIDGET_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ml/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hpc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/system/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/research/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/widget_registry.cpp"
)

add_library(straylight-widgets STATIC ${WIDGET_SOURCES})

target_include_directories(straylight-widgets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(straylight-widgets PUBLIC
    straylight-common
    imgui
)

if(NOT STRAYLIGHT_BUILD_DESKTOP_ONLY)
    install(TARGETS straylight-widgets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# Standalone widget launcher.
#
# The widgets self-register through static initializers in each widget .cpp.
# If the launcher only links the static archive, the linker can discard those
# object files because no ordinary symbol is referenced from them. Compile the
# widget sources into the launcher so `--list` and named launches expose the
# actual registry.
find_package(PkgConfig QUIET)
find_package(OpenGL QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GLFW3 QUIET glfw3)
endif()
find_path(STRAYLIGHT_IMGUI_BACKENDS_DIR imgui_impl_glfw.h
    PATHS
        /usr/include/imgui/backends
        /usr/local/include/imgui/backends
)
find_file(STRAYLIGHT_IMGUI_IMPL_GLFW_SRC imgui_impl_glfw.cpp
    PATHS
        /usr/share/doc/libimgui-dev/examples/backends
        /usr/local/src/imgui/backends
)
find_file(STRAYLIGHT_IMGUI_IMPL_OPENGL3_SRC imgui_impl_opengl3.cpp
    PATHS
        /usr/share/doc/libimgui-dev/examples/backends
        /usr/local/src/imgui/backends
)

set(STRAYLIGHT_WIDGET_LAUNCHER_GUI OFF)
if(GLFW3_FOUND
   AND OpenGL_FOUND
   AND STRAYLIGHT_IMGUI_BACKENDS_DIR
   AND STRAYLIGHT_IMGUI_IMPL_GLFW_SRC
   AND STRAYLIGHT_IMGUI_IMPL_OPENGL3_SRC)
    set(STRAYLIGHT_WIDGET_LAUNCHER_GUI ON)
endif()

add_executable(straylight-widget-launcher
    main.cpp
    ${WIDGET_SOURCES}
)
if(STRAYLIGHT_WIDGET_LAUNCHER_GUI)
    target_sources(straylight-widget-launcher PRIVATE
        ${STRAYLIGHT_IMGUI_IMPL_GLFW_SRC}
        ${STRAYLIGHT_IMGUI_IMPL_OPENGL3_SRC}
    )
endif()

target_include_directories(straylight-widget-launcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
if(STRAYLIGHT_WIDGET_LAUNCHER_GUI)
    target_include_directories(straylight-widget-launcher PRIVATE
        ${STRAYLIGHT_IMGUI_BACKENDS_DIR}
        ${GLFW3_INCLUDE_DIRS}
    )
    target_link_directories(straylight-widget-launcher PRIVATE
        ${GLFW3_LIBRARY_DIRS}
    )
else()
    target_compile_definitions(straylight-widget-launcher PRIVATE
        STRAYLIGHT_WIDGET_LAUNCHER_HEADLESS=1
    )
endif()

target_link_libraries(straylight-widget-launcher PRIVATE
    straylight-common
    imgui
)
if(STRAYLIGHT_WIDGET_LAUNCHER_GUI)
    target_link_libraries(straylight-widget-launcher PRIVATE
        ${GLFW3_LIBRARIES}
        OpenGL::GL
    )
endif()

if(STRAYLIGHT_WIDGET_LAUNCHER_GUI)
    message(STATUS "StrayLight widget launcher: Debian GLFW GUI backend enabled")
else()
    message(STATUS "StrayLight widget launcher: headless registry backend enabled")
endif()

install(TARGETS straylight-widget-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
