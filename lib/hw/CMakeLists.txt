add_library(straylight-hw SHARED
    src/gpu.cpp
    src/mgpu.cpp
    src/entropy.cpp
)

target_include_directories(straylight-hw
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(straylight-hw PUBLIC straylight-common)

set_target_properties(straylight-hw PROPERTIES
    VERSION ${STRAYLIGHT_VERSION}
    SOVERSION ${STRAYLIGHT_SO_VERSION}
)

target_compile_definitions(straylight-hw PRIVATE straylight_hw_EXPORTS)

# x86 intrinsics for RDRAND/RDSEED entropy
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-mrdrnd HAS_RDRND)
if(HAS_RDRND)
    target_compile_options(straylight-hw PRIVATE -mrdrnd)
endif()

if(STRAYLIGHT_BUILD_LIBS_ONLY OR NOT STRAYLIGHT_PACKAGE_COMPONENT_BUILD)
    install(TARGETS straylight-hw
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
