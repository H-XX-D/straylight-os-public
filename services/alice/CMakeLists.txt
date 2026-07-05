add_executable(straylight-alice
    main.cpp
    alice_daemon.cpp
    alice_engine.cpp
    log_analyzer.cpp
    alert_manager.cpp
    ipc_server.cpp
)

target_include_directories(straylight-alice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-alice PRIVATE straylight-common)

# Optional llama.cpp support — graceful fallback to rule-based analysis without it
find_library(LLAMA_LIB llama QUIET)
find_path(LLAMA_INCLUDE llama.h QUIET)

if(LLAMA_LIB AND LLAMA_INCLUDE)
    message(STATUS "  Alice: llama.cpp found — GGUF inference enabled")
    target_compile_definitions(straylight-alice PRIVATE HAVE_LLAMA_CPP)
    target_include_directories(straylight-alice PRIVATE ${LLAMA_INCLUDE})
    target_link_libraries(straylight-alice PRIVATE ${LLAMA_LIB})
else()
    message(STATUS "  Alice: llama.cpp not found — using rule-based analysis fallback")
endif()

install(TARGETS straylight-alice DESTINATION bin)
