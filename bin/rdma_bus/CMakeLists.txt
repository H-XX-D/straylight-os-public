find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

add_executable(straylight-rdma-bus
    main.cpp
    verbs.cpp
    memory_region.cpp
    queue_pair.cpp
    tensor_rdma.cpp)

target_link_libraries(straylight-rdma-bus PRIVATE
    straylight-common
    ${IBVERBS_LIBRARIES}
    ibverbs
    rdmacm)

target_include_directories(straylight-rdma-bus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IBVERBS_INCLUDE_DIRS})

target_compile_options(straylight-rdma-bus PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS straylight-rdma-bus DESTINATION bin)
