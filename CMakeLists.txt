cmake_minimum_required(VERSION 3.20)
project(StreamForge LANGUAGES CXX)

include(CTest)

option(STREAMFORGE_BUILD_RECORDING_PIPELINE_TESTS
       "Build the standalone RecordingPipeline lifecycle tests" ON)
option(STREAMFORGE_WITH_FFMPEG
       "Build the FFmpeg-backed Matroska/MP4 muxer adapter" OFF)

if(BUILD_TESTING AND STREAMFORGE_BUILD_RECORDING_PIPELINE_TESTS)
    add_subdirectory(tests)
endif()

if(WIN32)
    add_library(streamforge_wasapi_capture
        src/capture/WasapiAudioCaptureSource.cpp
        src/capture/WasapiAudioCaptureSource.h
    )
    target_compile_features(streamforge_wasapi_capture PUBLIC cxx_std_20)
    target_include_directories(streamforge_wasapi_capture
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src/capture
    )
    target_link_libraries(streamforge_wasapi_capture PRIVATE ole32 uuid)
    target_compile_options(streamforge_wasapi_capture PRIVATE /W4 /permissive-)

    add_library(streamforge_dxgi_capture
        src/capture/DxgiDesktopCaptureSource.cpp
        src/capture/DxgiDesktopCaptureSource.h
    )
    target_compile_features(streamforge_dxgi_capture PUBLIC cxx_std_20)
    target_include_directories(streamforge_dxgi_capture
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src/capture
    )
    target_link_libraries(streamforge_dxgi_capture PRIVATE d3d11 dxgi ole32 uuid)
    target_compile_options(streamforge_dxgi_capture PRIVATE /W4 /permissive-)
endif()

if(STREAMFORGE_WITH_FFMPEG)
    find_package(Threads REQUIRED)
    if(WIN32)
        # vcpkg's FFmpeg port supplies the FFMPEG CMake find-module.
        find_package(FFMPEG REQUIRED)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil
            libswresample
            libswscale
        )
    endif()

    add_library(streamforge_ffmpeg_muxer
        src/recording/FFmpegMediaMuxer.cpp
        src/recording/FFmpegMediaMuxer.h
        src/recording/FFmpegAudioEncoder.cpp
        src/recording/FFmpegAudioEncoder.h
        src/recording/FFmpegVideoEncoder.cpp
        src/recording/FFmpegVideoEncoder.h
        src/recording/FFmpegRecordingFactories.cpp
        src/recording/FFmpegRecordingFactories.h
    )
    target_compile_features(streamforge_ffmpeg_muxer PUBLIC cxx_std_20)
    target_include_directories(streamforge_ffmpeg_muxer
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src/recording
    )
    if(WIN32)
        target_include_directories(streamforge_ffmpeg_muxer PUBLIC ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(streamforge_ffmpeg_muxer PUBLIC ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(streamforge_ffmpeg_muxer PUBLIC ${FFMPEG_LIBRARIES})
    else()
        target_link_libraries(streamforge_ffmpeg_muxer PUBLIC PkgConfig::FFMPEG)
    endif()

    if(MSVC)
        target_compile_options(streamforge_ffmpeg_muxer PRIVATE /W4 /permissive-)
    else()
        target_compile_options(streamforge_ffmpeg_muxer PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()

    if(BUILD_TESTING)
        add_executable(streamforge_ffmpeg_muxer_tests
            tests/FFmpegMuxerIntegrationTests.cpp
            src/recording/RecordingPipeline.cpp
        )
        target_compile_features(streamforge_ffmpeg_muxer_tests PRIVATE cxx_std_20)
        target_include_directories(streamforge_ffmpeg_muxer_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/recording
        )
        target_link_libraries(streamforge_ffmpeg_muxer_tests PRIVATE
            streamforge_ffmpeg_muxer
            Threads::Threads
        )
        add_test(NAME ffmpeg_muxer_matroska_integration
                 COMMAND streamforge_ffmpeg_muxer_tests)
    endif()
endif()
