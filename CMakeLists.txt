cmake_minimum_required(VERSION 3.20)
project(StreamForge LANGUAGES CXX)

include(CTest)

option(STREAMFORGE_BUILD_RECORDING_PIPELINE_TESTS
       "Build the standalone RecordingPipeline lifecycle tests" ON)

if(BUILD_TESTING AND STREAMFORGE_BUILD_RECORDING_PIPELINE_TESTS)
    add_subdirectory(tests)
endif()
