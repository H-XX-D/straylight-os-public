add_library(straylight-net SHARED
    src/socket.cpp
    src/transport.cpp
)

target_include_directories(straylight-net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(straylight-net PUBLIC straylight-common)

set_target_properties(straylight-net PROPERTIES
    VERSION ${STRAYLIGHT_VERSION}
    SOVERSION ${STRAYLIGHT_SO_VERSION}
)

target_compile_definitions(straylight-net PRIVATE straylight_net_EXPORTS)

if(STRAYLIGHT_BUILD_LIBS_ONLY OR NOT STRAYLIGHT_PACKAGE_COMPONENT_BUILD)
    install(TARGETS straylight-net
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
