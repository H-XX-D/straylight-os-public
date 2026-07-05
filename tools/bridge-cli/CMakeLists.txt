cmake_minimum_required(VERSION 3.25)
project(bridge-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# bridge-cli communicates with the daemon via control files,
# so it doesn't link the engine directly — it's a lightweight client.
add_executable(bridge-cli
    main.cpp
)

target_include_directories(bridge-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../services/bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/../../straylight-os/include
)

target_link_libraries(bridge-cli PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(bridge-cli PRIVATE rt)
endif()

install(TARGETS bridge-cli RUNTIME DESTINATION bin)
