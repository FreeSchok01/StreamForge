# StreamForge media clock and A/V timestamp contract

## Session origin

`RecordingPipelineConfig::timestampOriginNs` defines the common monotonic clock
origin for one recording. If it is `kNoTimestamp`, the pipeline captures a
`std::chrono::steady_clock` timestamp during start and passes the same origin
to both encoder configurations.

Callers may provide an explicit origin when capture devices and the pipeline
share a clock established before recording starts.

## Audio timestamps

- `AudioFrame::timestampNs` is the monotonic timestamp of the first sample in
  the frame.
- The FFmpeg audio encoder converts the offset from the shared origin into the
  output sample timebase.
- Consecutive audio frames must be contiguous within a 5 ms tolerance.
- A larger gap or overlap fails the encoder instead of silently hiding lost or
  duplicated audio.
- Resampling delay and encoder packet delay are handled inside the audio adapter.

## Video timestamps

- `VideoFrameInput::timestampNs`, when supplied, is the capture timestamp and
  takes precedence over the frame's legacy `pts`.
- The timestamp offset from the same session origin is rescaled into the video
  encoder timebase.
- If no capture timestamp is supplied, the adapter uses `pts` and `timebase`.
- Video PTS must increase strictly.

## Capture-source requirement

WASAPI and DXGI capture adapters must normalize their timestamps into one shared
monotonic domain before submitting frames. Wall-clock time (UTC/local time) must
not be used for media timestamps. QPC values may be used only after conversion
into the same nanosecond clock domain used by the pipeline.

## Current limitations

This establishes a common PTS origin but is not yet a complete drift-correction
system. There is no automatic audio gap insertion, video frame duplication/drop
correction, or long-term device-clock drift compensation. Those behaviors will
be implemented with the capture graph and A/V synchronizer.
