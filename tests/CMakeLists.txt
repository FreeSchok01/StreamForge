add_executable(streamforge_recording_pipeline_tests
    RecordingPipelineTests.cpp
)
target_compile_features(streamforge_recording_pipeline_tests PRIVATE cxx_std_20)
target_include_directories(streamforge_recording_pipeline_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/recording
)
target_link_libraries(streamforge_recording_pipeline_tests PRIVATE streamforge_recording_core)

if(MSVC)
    target_compile_options(streamforge_recording_pipeline_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(streamforge_recording_pipeline_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

add_test(NAME recording_pipeline_lifecycle COMMAND streamforge_recording_pipeline_tests)
