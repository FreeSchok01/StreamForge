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
- FFmpeg NVIDIA NVENC backend for H.264, HEVC and AV1 when supported by the installed FFmpeg build and GPU.
- FFmpeg Matroska/MP4 muxer adapter.
- Mock-based lifecycle tests and a real AAC + H.264 Matroska end-to-end test.

WASAPI microphone/loopback capture, active endpoint enumeration and DXGI
desktop capture are implemented as Windows-only sources and are coordinated
by a Windows recording-session controller. DXGI can retain an owned D3D11 BGRA texture and its D3D11 device in a shared
COM lifetime lease alongside the optional CPU staging-readback frame, so GPU
consumers can access the resource across the dispatcher handoff. CPU readback
and native texture retention are independently configurable.
The media frame contract also accepts native-only D3D11 BGRA frames. The FFmpeg
NVENC adapter supports H.264, HEVC and AV1 hardware compression from CPU frames;
it does not yet import the retained D3D11 texture directly. FFmpeg capability
reporting includes build-time encoder presence and advertised D3D11VA/CUDA
hardware-frame configurations, but does not probe the installed GPU or driver.
A Windows D3D11 video-processor capability probe now checks BGRA8 input and
NV12 output support as a preflight for the future GPU conversion stage; it does
not perform conversion yet. Direct D3D11-to-NVENC frame submission, AMD AMF,
Intel oneVPL/QSV, desktop UI
and Twitch output are not yet implemented.

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

## Build: Windows with FFmpeg

The repository includes a vcpkg manifest for FFmpeg with x264 support. With
vcpkg installed and `VCPKG_ROOT` set:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DSTREAMFORGE_WITH_FFMPEG=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The vcpkg manifest pins its registry baseline and enables the GPL x264 feature,
consistent with this repository's GPL-3.0 license. The FFmpeg DLL directory
(`vcpkg_installed/x64-windows/bin`) must be on `PATH` when running the tests.

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
