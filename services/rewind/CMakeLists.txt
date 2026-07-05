cmake_minimum_required(VERSION 3.25)
project(straylight-rewind LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Include paths ────────────────────────────────────────────────────
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# ── Library: rewind-core ─────────────────────────────────────────────
add_library(rewind-core STATIC
    checkpoint_store.cpp
    checkpoint_engine.cpp
    process_monitor.cpp
)

target_include_directories(rewind-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# Optional zstd support
find_library(ZSTD_LIB zstd)
if(ZSTD_LIB)
    target_link_libraries(rewind-core PUBLIC ${ZSTD_LIB})
    target_compile_definitions(rewind-core PUBLIC STRAYLIGHT_HAS_ZSTD)
endif()

# ── Daemon executable ────────────────────────────────────────────────
add_executable(straylight-rewind main.cpp)
target_link_libraries(straylight-rewind PRIVATE rewind-core)

# ── Install ──────────────────────────────────────────────────────────
install(TARGETS straylight-rewind DESTINATION sbin)
install(DIRECTORY DESTINATION /var/lib/straylight/rewind)
