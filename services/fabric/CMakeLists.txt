cmake_minimum_required(VERSION 3.25)
# Note: when built as sub-project, project() is a no-op for the parent's settings.
project(straylight-fabric LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Library: fabric-core ─────────────────────────────────────────────
add_library(fabric-core STATIC
    topology.cpp
    query_engine.cpp
    udev_monitor.cpp
)

target_include_directories(fabric-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(fabric-core PUBLIC straylight-common)

# ── Daemon executable ────────────────────────────────────────────────
add_executable(straylight-fabric main.cpp)
target_include_directories(straylight-fabric PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-fabric PRIVATE fabric-core straylight-common)

# pthreads for udev monitor thread
find_package(Threads REQUIRED)
target_link_libraries(straylight-fabric PRIVATE Threads::Threads)
target_link_libraries(fabric-core PUBLIC Threads::Threads)

# ── Install ──────────────────────────────────────────────────────────
install(TARGETS straylight-fabric DESTINATION sbin)
install(DIRECTORY DESTINATION /var/lib/straylight/fabric)
