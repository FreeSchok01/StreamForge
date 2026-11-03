# DXGI Desktop Duplication capture

## Current implementation

`DxgiDesktopCaptureSource` captures a selected DXGI output using
`IDXGIOutputDuplication`. The caller selects an adapter and output index.
Frames are acquired on a dedicated worker thread and copied into owned BGRA8
CPU memory using a D3D11 staging texture.

Each `OwnedVideoFrame` includes the DXGI present timestamp converted from QPC
ticks into nanoseconds, allowing the FFmpeg video encoder to derive PTS from
the shared session clock.

## Connecting to the recording dispatcher

Start the pipeline and dispatcher before starting capture:

```cpp
DxgiDesktopCaptureSource capture;
VideoCaptureConfig captureConfig;
captureConfig.adapterIndex = 0;
captureConfig.outputIndex = 0;

if (!capture.start(captureConfig,
    [&dispatcher](OwnedVideoFrame&& frame, std::string& error) {
        const bool accepted = dispatcher.trySubmitVideo(std::move(frame));
        if (!accepted &&
            dispatcher.snapshot().state == RecordingInputDispatcherState::Failed) {
            error = dispatcher.snapshot().error;
            return false;
        }
        // A false trySubmitVideo while the dispatcher remains Running means
        // the bounded video queue dropped this newest frame. Keep capturing.
        return true;
    }, error)) {
    // Handle startup error.
}
```

Stop order: stop DXGI and WASAPI capture sources first, then stop the
RecordingInputDispatcher. A graceful dispatcher stop drains accepted frames
before finalizing the recording.

## Limitations

- The first implementation uses GPU-to-CPU staging readback on every frame. It
  is a functional baseline, not the final GPU-first rendering path.
- DXGI access-lost/device-change errors are reported; automatic duplication
  recreation is not implemented yet.
- Output rotation and cursor composition are not implemented.
- Adapter/output enumeration and UI selection are not implemented; callers
  currently supply numeric adapter/output indices.
- Windows CI compiles the source, but no physical desktop capture runtime test
  is performed in CI.
