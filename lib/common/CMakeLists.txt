find_package(spdlog 1.10 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(fmt QUIET)

add_library(straylight-common SHARED
    src/log.cpp
    src/config.cpp
    src/ipc.cpp
    src/daemon.cpp
)

target_include_directories(straylight-common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(straylight-common
    PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json
)

if(fmt_FOUND)
    target_link_libraries(straylight-common PUBLIC fmt::fmt)
endif()

set_target_properties(straylight-common PROPERTIES
    VERSION ${STRAYLIGHT_VERSION}
    SOVERSION ${STRAYLIGHT_SO_VERSION}
)

target_compile_definitions(straylight-common PRIVATE straylight_common_EXPORTS)

if(STRAYLIGHT_BUILD_LIBS_ONLY OR NOT STRAYLIGHT_PACKAGE_COMPONENT_BUILD)
    install(TARGETS straylight-common
        EXPORT StraylightTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )
endif()
