# RecordingPipeline lifecycle tests

These tests use mock encoders and a mock muxer. They validate the session lifecycle
without requiring Qt, FFmpeg, capture hardware, or a GPU.

Build from repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
