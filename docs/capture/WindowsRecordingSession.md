# WindowsRecordingSession

`WindowsRecordingSession` coordinates the first complete Windows local-recording
path:

`WASAPI -> RecordingInputDispatcher -> FFmpeg encoders -> FFmpeg muxer`

and

`DXGI Desktop Duplication -> RecordingInputDispatcher -> FFmpeg video encoder`.

## Startup order

1. Query the selected WASAPI endpoint mix format.
2. Apply the actual capture sample rate and channel count to the audio encoder input.
3. Choose a shared monotonic timestamp origin. By default the session places it
   one audio buffer plus 50 ms before startup to tolerate buffered capture packets.
4. Start RecordingPipeline and RecordingInputDispatcher.
5. Start WASAPI and DXGI capture workers.
6. Verify that both capture workers and both dispatcher workers are still alive
   before reporting successful session startup. A worker that exits immediately
   after its startup handshake causes startup to be aborted and the pipeline
   to be cleaned up.

## Runtime health

`snapshot()` exposes whether the audio/video capture workers and dispatcher
are still running. It also reports `captureFailureDetected` and a
`captureError` message if one of those workers stops unexpectedly. The
application should react by stopping the session (normally in abort mode) and
presenting the diagnostic; the coordinator does not silently continue with a
missing audio or video stream.

## Shutdown order

1. Stop DXGI and WASAPI capture workers.
2. Graceful mode drains the accepted SPSC input frames.
3. Stop the dispatcher, which flushes encoders and finalizes the muxer.
4. Abort mode discards pending input frames and aborts the muxer.

## Example

```cpp
WindowsRecordingSession session(makeFFmpegRecordingPipelineFactories());
WindowsRecordingSessionConfig config;

config.recording.output.outputPath = "recording.mkv";
config.recording.output.container = RecordingContainer::Matroska;
config.audioCapture.loopback = false; // microphone; true for desktop audio
config.videoCapture.adapterIndex = 0;
config.videoCapture.outputIndex = 0;

std::string error;
if (!session.start(config, error)) {
    // Handle startup error.
}

// When the user stops recording:
if (!session.stop(RecordingStopMode::Graceful, error)) {
    // Inspect the reported capture/recording error.
}
```

## Current limits

This coordinator is a Windows-only integration baseline. It currently uses
software FFmpeg encoding and CPU-owned BGRA frames. The session disables
`includeNativeResource` because its current FFmpeg encoder does not consume the
extra D3D11 texture; standalone DXGI consumers can enable it when needed.
The current FFmpeg encoder adapter consumes CPU pixels. Consequently,
`WindowsRecordingSession::start()` rejects `videoCapture.includeCpuReadback=false`
up front with an actionable error; native-only DXGI frames are currently for
future GPU-native consumers, not this FFmpeg session. This is not yet a
zero-copy or direct D3D11-to-NVENC encoding path. The coordinator does not yet
integrate the Qt studio UI, hardware encoders, automatic device hot-plug
recovery or Twitch output. WASAPI endpoint-change notifications stop capture safely, but the user must reselect the device and restart. Real hardware testing is still
required on physical microphone, loopback and display devices.
