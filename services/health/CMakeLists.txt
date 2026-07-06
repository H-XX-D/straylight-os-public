add_executable(straylight-healthd
    main.cpp
    health_scorer.cpp
    checks.cpp
)
target_include_directories(straylight-healthd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-healthd PRIVATE straylight-common)

# macOS frameworks for battery/VM stats
if(APPLE)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(CF_FRAMEWORK CoreFoundation)
    if(IOKIT_FRAMEWORK AND CF_FRAMEWORK)
        target_link_libraries(straylight-healthd PRIVATE ${IOKIT_FRAMEWORK} ${CF_FRAMEWORK})
    endif()
endif()

install(TARGETS straylight-healthd DESTINATION lib/straylight)
install(FILES etc/straylight/health.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/straylight)
