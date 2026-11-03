# StreamForge

StreamForge is an independent open-source streaming studio and media engine.
The repository is being bootstrapped around a standalone C++ recording engine.

## Current implementation

- Shared media, audio/video frame, encoder and muxer contracts under `include/core/`.
- Recording orchestration under `src/recording/`.
- Bounded encoded-packet queue and a dedicated single-owner muxer worker.
- Asynchronous capture-to-encoder dispatcher with SPSC audio/video queues.
- Input/callback admission gates, graceful finalization, abort and error propagation.
- FFmpeg audio encoder with persistent resampling and sample FIFO.
- FFmpeg software video encoder with pixel-format conversion.
- FFmpeg Matroska/MP4 muxer adapter.
- Mock-based lifecycle tests and a real AAC + H.264 Matroska end-to-end test.

WASAPI microphone/loopback and DXGI desktop capture are implemented as
Windows-only sources. DXGI currently uses CPU staging readback; GPU-native
texture handoff, hardware encoder adapters, desktop UI and Twitch output are
not yet implemented.

## Build: core lifecycle tests

Requirements: CMake 3.20+, a C++20 compiler and platform thread support.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Build: FFmpeg recording adapters

Install FFmpeg development packages for libavformat, libavcodec, libavutil,
libswresample and libswscale, plus pkg-config. On Ubuntu:

```sh
sudo apt-get install pkg-config libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev
cmake -S . -B build-ffmpeg -DCMAKE_BUILD_TYPE=Debug -DSTREAMFORGE_WITH_FFMPEG=ON
cmake --build build-ffmpeg --parallel
ctest --test-dir build-ffmpeg --output-on-failure
```

The FFmpeg-enabled integration test creates a real recording through
`makeFFmpegRecordingPipelineFactories()`, writes AAC audio and H.264 video to
Matroska, reopens the file and verifies both streams. The FFmpeg-enabled CI job
currently runs on Ubuntu; Windows CI validates the core lifecycle only.

## Threading contract

`submitAudioFrame()` and `submitVideoFrame()` call their encoders synchronously.
Call them from dedicated media-processing threads, never directly from WASAPI or
DXGI capture callbacks. Encoders must propagate packet-callback rejection and
quiesce callbacks before `shutdown()` returns. All muxer methods are owned by
the muxer worker thread.

The current reference integration test uses synthetic frames; it does not yet
validate live capture, long-duration stability, hardware encoding or Twitch output.

## License

See [LICENSE](LICENSE). The repository is licensed under GPL-3.0.
