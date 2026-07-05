# Shared compiler flags for all StrayLight targets

# Strict warnings
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Werror=return-type
    -Werror=uninitialized
)

# Symbol visibility: hidden by default
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Position-independent code for shared libraries
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sanitizers in Debug mode
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(ENABLE_SANITIZERS "Enable ASan + UBSan" ON)
    if(ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# LTO in Release mode
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()


# Dear ImGui is provided by the distribution package. Treat it as an imported
# target so app targets inherit the include directory as well as the archive path.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(STRAYLIGHT_IMGUI_PC QUIET imgui)
endif()

find_path(STRAYLIGHT_IMGUI_INCLUDE_DIR imgui.h
    PATHS ${STRAYLIGHT_IMGUI_PC_INCLUDE_DIRS} /usr/include/imgui
    NO_DEFAULT_PATH
)
find_path(STRAYLIGHT_IMGUI_BACKENDS_INCLUDE_DIR imgui_impl_opengl3.h
    PATHS
        ${STRAYLIGHT_IMGUI_INCLUDE_DIR}/backends
        /usr/include/imgui/backends
        /usr/local/include/imgui/backends
        /usr/local/include/imgui
    NO_DEFAULT_PATH
)
find_library(STRAYLIGHT_IMGUI_LIBRARY imgui
    PATHS ${STRAYLIGHT_IMGUI_PC_LIBRARY_DIRS}
          /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
          /usr/lib
    NO_DEFAULT_PATH
)
if(STRAYLIGHT_IMGUI_INCLUDE_DIR AND STRAYLIGHT_IMGUI_LIBRARY AND NOT TARGET imgui)
    set(STRAYLIGHT_IMGUI_LINK_LIBRARIES ${STRAYLIGHT_IMGUI_PC_LIBRARIES})
    list(REMOVE_ITEM STRAYLIGHT_IMGUI_LINK_LIBRARIES imgui)
    set(STRAYLIGHT_IMGUI_INCLUDE_DIRS
        ${STRAYLIGHT_IMGUI_INCLUDE_DIR}
        ${STRAYLIGHT_IMGUI_PC_INCLUDE_DIRS}
    )
    if(STRAYLIGHT_IMGUI_BACKENDS_INCLUDE_DIR)
        list(APPEND STRAYLIGHT_IMGUI_INCLUDE_DIRS ${STRAYLIGHT_IMGUI_BACKENDS_INCLUDE_DIR})
    endif()

    add_library(imgui STATIC IMPORTED GLOBAL)
    set_target_properties(imgui PROPERTIES
        IMPORTED_LOCATION "${STRAYLIGHT_IMGUI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${STRAYLIGHT_IMGUI_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${STRAYLIGHT_IMGUI_LINK_LIBRARIES}"
    )
endif()

# Helper function to create a StrayLight shared library
function(straylight_add_library target_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_HEADERS;DEPS" ${ARGN})

    add_library(${target_name} SHARED ${ARG_SOURCES})

    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    set_target_properties(${target_name} PROPERTIES
        VERSION ${STRAYLIGHT_VERSION}
        SOVERSION ${STRAYLIGHT_SO_VERSION}
        EXPORT_NAME ${target_name}
    )

    if(ARG_DEPS)
        target_link_libraries(${target_name} PUBLIC ${ARG_DEPS})
    endif()
endfunction()
