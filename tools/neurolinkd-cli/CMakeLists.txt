add_executable(straylight-neurolinkd
    main.cpp
)
target_link_libraries(straylight-neurolinkd PRIVATE straylight-common)
target_include_directories(straylight-neurolinkd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/http
)
target_sources(straylight-neurolinkd PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/http/http_client.cpp
)
install(TARGETS straylight-neurolinkd DESTINATION bin)
