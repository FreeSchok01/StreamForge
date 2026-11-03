# StreamForge

StreamForge is an independent open-source streaming studio and media engine.
The repository is being bootstrapped. The first implementation milestone is
the recording-pipeline lifecycle layer.

## Current repository scope

- Shared media and encoder contracts under `include/core/`.
- Recording orchestration under `src/recording/`.
- A bounded encoded-packet queue and dedicated muxer worker.
- Input/callback admission gates, graceful finalization, abort and error propagation.
- Mock-based lifecycle tests that do not require Qt or FFmpeg.

FFmpeg encoder adapters, the FFmpeg muxer, capture backends, desktop UI and Twitch
output are not yet integrated.

## Build and test

Requirements: CMake 3.20+, a C++20 compiler and platform thread support.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Threading contract

`submitAudioFrame()` and `submitVideoFrame()` call their encoders synchronously.
Call them from dedicated media-processing threads, never directly from WASAPI or
DXGI capture callbacks. Encoders must propagate packet-callback rejection and
quiesce callbacks before `shutdown()` returns. All muxer methods are owned by
the muxer worker thread.

## License

See [LICENSE](LICENSE). The repository is licensed under GPL-3.0.
