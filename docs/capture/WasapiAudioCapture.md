# WASAPI audio capture

## Current implementation

`WasapiAudioCaptureSource` is an event-driven Windows audio capture source.
It supports the default or an explicitly selected endpoint in either mode:

- microphone/line input (`eCapture`)
- desktop/system loopback (`eRender` + `AUDCLNT_STREAMFLAGS_LOOPBACK`)

The source reads the endpoint mix format and converts Float32 or signed PCM
16/24/32-bit samples into interleaved Float32 `AudioFrame` objects. Supported
channel counts are mono, stereo, 5.1 and 7.1. Capture timestamps use the WASAPI
QPC position converted to nanoseconds.

## Connecting to the recording dispatcher

Query the endpoint format first so the audio encoder input configuration
matches the actual Windows mix format. The capture source converts that format
to Float32 interleaved samples:

```cpp
WasapiAudioCaptureSource capture;
AudioCaptureConfig captureConfig;
captureConfig.loopback = false; // true for desktop/system audio

AudioCaptureFormat captureFormat;
if (!capture.queryFormat(captureConfig, captureFormat, error)) {
    // Handle format query error.
}
recordingConfig.audio.inputSampleRate = captureFormat.sampleRate;
recordingConfig.audio.inputChannels = captureFormat.channels;

// Then start RecordingPipeline and RecordingInputDispatcher before capture.
if (!capture.start(captureConfig,
    [&dispatcher](AudioFrame&& frame, std::string&) {
        return dispatcher.trySubmitAudio(std::move(frame));
    }, error)) {
    // Handle startup error.
}
```

For loopback capture, set `loopback = true`. The capture callback only transfers
the owned frame into the dispatcher's SPSC queue; encoding and muxing happen on
separate worker threads.

## Limitations

- Device enumeration and UI selection are not implemented yet; callers may pass
  a known endpoint ID or use the Windows default endpoint.
- Device invalidation currently reports an error and stops capture. Automatic
  hot-plug recovery and endpoint-change notifications are future work.
- The Windows CI job compiles this source but does not run it against physical
  audio hardware. Real microphone and loopback testing is still required.
