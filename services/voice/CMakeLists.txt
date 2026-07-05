cmake_minimum_required(VERSION 3.25)
project(straylight-voice LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# StrayLight shared include path
set(STRAYLIGHT_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../straylight-os/include")

add_executable(straylight-voice
    main.cpp
    audio_capture.cpp
    stt_engine.cpp
    tts_engine.cpp
    llm_engine.cpp
    tool_executor.cpp
    conversation.cpp
)

target_include_directories(straylight-voice PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${STRAYLIGHT_INCLUDE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(straylight-voice PRIVATE Threads::Threads)

# Optional: ALSA for audio capture/playback on Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA alsa)
        if(ALSA_FOUND)
            target_include_directories(straylight-voice PRIVATE ${ALSA_INCLUDE_DIRS})
            target_link_libraries(straylight-voice PRIVATE ${ALSA_LIBRARIES})
        endif()
    endif()
    target_link_libraries(straylight-voice PRIVATE rt)
endif()

# Optional: whisper.cpp for native STT
option(VOICE_USE_WHISPER_CPP "Link against whisper.cpp for native STT" OFF)
if(VOICE_USE_WHISPER_CPP)
    find_library(WHISPER_LIB whisper)
    if(WHISPER_LIB)
        target_compile_definitions(straylight-voice PRIVATE HAVE_WHISPER_CPP)
        target_link_libraries(straylight-voice PRIVATE ${WHISPER_LIB})
    endif()
endif()

# Optional: llama.cpp for native LLM
option(VOICE_USE_LLAMA_CPP "Link against llama.cpp for native LLM" OFF)
if(VOICE_USE_LLAMA_CPP)
    find_library(LLAMA_LIB llama)
    if(LLAMA_LIB)
        target_compile_definitions(straylight-voice PRIVATE HAVE_LLAMA_CPP)
        target_link_libraries(straylight-voice PRIVATE ${LLAMA_LIB})
    endif()
endif()

# Installation
install(TARGETS straylight-voice RUNTIME DESTINATION bin)
install(DIRECTORY DESTINATION /etc/straylight)
install(FILES ${CMAKE_SOURCE_DIR}/../../etc/straylight/voice.conf
        DESTINATION /etc/straylight
        COMPONENT config)
