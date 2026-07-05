find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf)
pkg_check_modules(LIBXDP libxdp)
find_program(CLANG_EXECUTABLE clang REQUIRED)

add_executable(straylight-xdp
    main.cpp
    loader.cpp
    maps.cpp
    af_xdp.cpp)

target_link_libraries(straylight-xdp PRIVATE
    straylight-common
    ${LIBBPF_LIBRARIES})

target_include_directories(straylight-xdp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBBPF_INCLUDE_DIRS})

if(LIBXDP_FOUND)
    target_link_libraries(straylight-xdp PRIVATE ${LIBXDP_LIBRARIES})
    target_include_directories(straylight-xdp PRIVATE ${LIBXDP_INCLUDE_DIRS})
    target_compile_definitions(straylight-xdp PRIVATE HAVE_LIBXDP=1)
endif()

set(STRAYLIGHT_XDP_BPF_SOURCES
    ${CMAKE_SOURCE_DIR}/kernel/xdp/xdp_filter.bpf.c
    ${CMAKE_SOURCE_DIR}/kernel/xdp/xdp_redirect.bpf.c
    ${CMAKE_SOURCE_DIR}/kernel/xdp/xdp_stats.bpf.c)

set(STRAYLIGHT_XDP_BPF_OBJECTS)
foreach(bpf_source IN LISTS STRAYLIGHT_XDP_BPF_SOURCES)
    get_filename_component(bpf_name "${bpf_source}" NAME)
    string(REGEX REPLACE "\\.c$" ".o" bpf_object_name "${bpf_name}")
    set(bpf_object "${CMAKE_CURRENT_BINARY_DIR}/${bpf_object_name}")
    add_custom_command(
        OUTPUT "${bpf_object}"
        COMMAND ${CLANG_EXECUTABLE}
            -O2 -g -target bpf -D__TARGET_ARCH_x86
            -I "${CMAKE_SOURCE_DIR}/kernel/xdp"
            -c "${bpf_source}" -o "${bpf_object}"
        DEPENDS "${bpf_source}" "${CMAKE_SOURCE_DIR}/kernel/xdp/xdp_maps.h"
        VERBATIM)
    list(APPEND STRAYLIGHT_XDP_BPF_OBJECTS "${bpf_object}")
endforeach()

add_custom_target(straylight-xdp-bpf ALL DEPENDS ${STRAYLIGHT_XDP_BPF_OBJECTS})
add_dependencies(straylight-xdp straylight-xdp-bpf)

target_compile_options(straylight-xdp PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS straylight-xdp DESTINATION bin)
install(FILES ${STRAYLIGHT_XDP_BPF_OBJECTS} DESTINATION lib/straylight/bpf)
