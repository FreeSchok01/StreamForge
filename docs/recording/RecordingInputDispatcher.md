# RecordingInputDispatcher contract

## Purpose

The dispatcher separates capture callbacks from encoder execution. Capture
producers move completed frames into bounded SPSC queues and return; dedicated
worker threads call `RecordingPipeline::submitAudioFrame()` and
`submitVideoFrame()`.

## Producer model

- Exactly one producer thread may submit audio frames to the audio queue.
- Exactly one producer thread may submit video frames to the video queue.
- Audio and video producers may run concurrently.
- The dispatcher owns one consumer thread per queue.
- The pipeline must already be in `Recording` state before dispatcher start.

## Queue policies

- Audio queue: bounded. Overflow is fatal because silently losing audio would
  corrupt the recording. The dispatcher stops accepting input and aborts the
  recording when stopped.
- Video queue: bounded, drop-newest. A full queue increments the dropped-frame
  counter but does not fail the recording.
- Graceful stop closes producer admission, waits for active submissions, drains
  accepted frames, then gracefully flushes/finalizes the RecordingPipeline.
- Abort closes admission, discards queued frames and aborts the RecordingPipeline.

## Frame ownership

AudioFrame already owns its interleaved sample storage. Video input is transferred
as `OwnedVideoFrame`, whose CPU planes remain alive until the worker's synchronous
encoder call returns. This first implementation is a CPU-frame handoff; D3D11
texture leases and GPU-native zero-copy transport are separate future work.

## Capture callback rules

Capture callbacks should only:
1. construct/copy a bounded frame payload,
2. call the corresponding `trySubmit...` method,
3. handle a false return (audio failure or video drop).

They must not call encoders, muxers, wait for worker completion, or perform disk I/O.
