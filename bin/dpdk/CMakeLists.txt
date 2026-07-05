find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

add_executable(straylight-dpdk
    main.cpp
    port.cpp
    pipeline.cpp
    flow.cpp
    tensor_transport.cpp)

target_link_libraries(straylight-dpdk PRIVATE
    straylight-common
    ${DPDK_LIBRARIES})

target_include_directories(straylight-dpdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(straylight-dpdk SYSTEM PRIVATE
    ${DPDK_INCLUDE_DIRS})

target_compile_options(straylight-dpdk PRIVATE
    -Wall -Wextra -Wpedantic
    ${DPDK_CFLAGS_OTHER})

install(TARGETS straylight-dpdk DESTINATION bin)
