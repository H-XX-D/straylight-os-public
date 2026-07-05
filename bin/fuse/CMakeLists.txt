# bin/fuse/CMakeLists.txt
# straylight-fuse — Tensor compression FUSE filesystem daemon.
# Requires libfuse3 and the lz4/zstd compression libraries.

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

add_executable(straylight-fuse
    main.cpp
    fuse_daemon.cpp
    tensor_fs.cpp
    operations.cpp
    compression.cpp
    tensor_format.cpp
    cache.cpp)

target_include_directories(straylight-fuse
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FUSE3_INCLUDE_DIRS})

target_link_libraries(straylight-fuse
    PRIVATE
        straylight-common
        straylight-ml
        ${FUSE3_LIBRARIES}
        lz4
        zstd)

# FUSE_USE_VERSION must be defined before any fuse header is included.
target_compile_definitions(straylight-fuse
    PRIVATE
        FUSE_USE_VERSION=35)

target_compile_options(straylight-fuse
    PRIVATE ${FUSE3_CFLAGS_OTHER})

install(TARGETS straylight-fuse RUNTIME DESTINATION bin)
