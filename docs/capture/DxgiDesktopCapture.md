# DXGI Desktop Duplication capture

## Current implementation

`DxgiDesktopCaptureSource` captures a selected DXGI output using
`IDXGIOutputDuplication`. The caller selects an adapter and output index.
Frames are acquired on a dedicated worker thread. Each frame is copied both to
owned BGRA8 CPU memory using a D3D11 staging texture and to a separate
D3D11 default-usage texture retained through a shared native-resource lease.
The lease survives `IDXGIOutputDuplication::ReleaseFrame()` and the dispatcher
queue handoff, allowing a future GPU consumer to access the texture safely.

Each `OwnedVideoFrame` includes the DXGI present timestamp converted from QPC
ticks into nanoseconds, allowing the FFmpeg video encoder to derive PTS from
the shared session clock. `VideoFrameInput` carries the native resource type,
D3D11 texture pointer, its owning D3D11 device pointer and a shared lifetime
owner that retains both COM objects. The current FFmpeg
software encoder ignores this native resource and consumes the CPU pixels; this
is not yet a zero-copy encoding path.

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

- CPU staging readback still occurs on every frame for the software encoder.
  The additional retained D3D11 texture is a GPU copy, not zero-copy capture.
- On `DXGI_ERROR_ACCESS_LOST`, duplication is recreated against the existing
  adapter/output with up to 20 retries at 250 ms intervals. Full adapter/output
  re-enumeration after topology changes is not implemented.
- Output rotation and cursor composition are not implemented.
- Adapter/output enumeration and UI selection are not implemented; callers
  currently supply numeric adapter/output indices.
- Windows CI compiles the source, but no physical desktop capture runtime test
  is performed in CI.
