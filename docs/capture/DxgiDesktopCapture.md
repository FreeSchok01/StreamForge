# DXGI Desktop Duplication capture

## Current implementation

`DxgiDesktopCaptureSource` captures a selected DXGI output using
`IDXGIOutputDuplication`. The caller selects an adapter and output index.
Frames are acquired on a dedicated worker thread. By default, each frame is
copied to owned BGRA8 CPU memory using a D3D11 staging texture and to a separate
D3D11 default-usage texture retained through a shared native-resource lease.
`VideoCaptureConfig::includeCpuReadback` and `includeNativeResource` independently
control those outputs; at least one must be enabled. The lease survives `IDXGIOutputDuplication::ReleaseFrame()` and the dispatcher
queue handoff, allowing a future GPU consumer to access the texture safely.

`VideoCaptureConfig::includeCpuReadback` defaults to `true` for compatibility
with the current FFmpeg CPU-input path. When set to `false` while
`includeNativeResource` remains enabled, DXGI skips staging texture creation,
GPU-to-CPU readback, mapping and CPU pixel allocation; the callback receives a
native-only BGRA D3D11 frame with its shared texture/device lease. Setting
`includeNativeResource=false` avoids the extra GPU texture allocation/copy when
no native GPU consumer is attached. This mode is suitable only when every downstream consumer accepts the
native D3D11 resource. The current FFmpeg encoder does not, so keep the default
for the existing FFmpeg recording session.

Each `OwnedVideoFrame` includes the DXGI present timestamp converted from QPC
ticks into nanoseconds, allowing the FFmpeg video encoder to derive PTS from
the shared session clock. `VideoFrameInput` carries the native resource type,
D3D11 texture pointer, its owning D3D11 device and immediate-context pointers,
a shared immediate-context mutex, and a lifetime owner retaining the COM
objects and mutex. The media frame contract also permits a D3D11 BGRA frame with no CPU planes,
so a future GPU encoder can avoid readback. The current FFmpeg software encoder
explicitly rejects native-only frames and consumes CPU pixels when provided;
this is not yet a zero-copy encoding path.
The lease retains the capture device's immediate context and a shared mutex.
DXGI locks this mutex around immediate-context operations, and downstream GPU
consumers must lock the same mutex around their operations. This serializes
access across the capture and consumer workers; D3D11 immediate contexts must
not be used concurrently from multiple threads. The lease does not grant
exclusive ownership of the context.

## D3D11 BGRA-to-NV12 conversion

`queryD3D11BgraToNv12Capabilities()` checks whether the selected D3D11 device
supports BGRA8 video-processor input and NV12 output for the requested
dimensions and frame rate.

`D3D11BgraToNv12Converter` now performs the conversion on that same device:
it creates a video-processor input view for the retained BGRA texture, an
NV12 output texture with render-target/video-encoder binding, an output view,
and submits `VideoProcessorBlt`. The output is a native-only
`OwnedVideoFrame` with `PixelFormat::NV12`; its lease retains the output
texture, device, immediate context and shared context mutex. No CPU readback is
performed by this conversion stage.

The current implementation is a first functional path, not yet a production
optimized one: it allocates an NV12 output texture and views for each converted
frame, uses progressive SDR BT.709 with studio-range NV12 output, and does not
pool surfaces or expose configurable colorimetry. The FFmpeg/NVENC adapter
still rejects native-only frames; the D3D11 NV12 output is not yet attached to
an FFmpeg `AVHWFramesContext` or submitted to NVENC. Device/driver runtime
validation is still required.

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

- CPU staging readback occurs by default for the existing FFmpeg encoder path.
  Native D3D11 retention can independently be disabled with
  `VideoCaptureConfig::includeNativeResource = false` to avoid an unused GPU copy.
  For a native GPU consumer, disable CPU readback while keeping native-resource
  retention enabled. The retained texture is a GPU copy, not zero-copy capture.
- On `DXGI_ERROR_ACCESS_LOST`, duplication is recreated against the existing
  adapter/output with up to 20 retries at 250 ms intervals. Full adapter/output
  re-enumeration after topology changes is not implemented.
- Output rotation and cursor composition are not implemented.
- Adapter/output enumeration and UI selection are not implemented; callers
  currently supply numeric adapter/output indices.
- Windows CI compiles the source, but no physical desktop capture runtime test
  is performed in CI.
