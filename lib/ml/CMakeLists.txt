add_library(straylight-ml SHARED
    src/tensor.cpp
    src/graph.cpp
    src/kv_cache.cpp
)

target_include_directories(straylight-ml
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(straylight-ml PUBLIC straylight-common)

set_target_properties(straylight-ml PROPERTIES
    VERSION ${STRAYLIGHT_VERSION}
    SOVERSION ${STRAYLIGHT_SO_VERSION}
)

target_compile_definitions(straylight-ml PRIVATE straylight_ml_EXPORTS)

if(STRAYLIGHT_BUILD_LIBS_ONLY OR NOT STRAYLIGHT_PACKAGE_COMPONENT_BUILD)
    install(TARGETS straylight-ml
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
