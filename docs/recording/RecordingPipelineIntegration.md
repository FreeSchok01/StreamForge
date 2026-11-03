# RecordingPipeline integration contract

## Lifecycle
1. Start validates configuration, initializes encoders, collects the actual stream descriptors and opens the muxer on its worker.
2. Callback and input gates open only after the muxer opens.
3. Input submission is synchronous and counted by the input gate. Stop closes admission and waits for accepted submissions.
4. Graceful stop flushes both encoders, shuts them down, closes and waits for callbacks, drains packets, and lets the muxer worker finalize.
5. Abort skips flush, shuts down encoders, closes callback admission, discards queued packets, and lets the muxer worker abort.

## Encoder contract
- initialize() must not emit packets.
- encode() may call the callback synchronously and must propagate callback rejection.
- flush() emits all delayed packets before returning.
- shutdown() is idempotent and guarantees there are no callbacks after it returns.
- Asynchronous callbacks after encode()/flush() return are unsupported in this version.

## Muxer contract
All muxer methods (configure/open/write/finalize/abort) run only on the muxer worker. abort() must be idempotent and safe after partial initialization. writePacket() consumes or copies packet bytes before returning.

## Capture-thread boundary
submitAudioFrame() and submitVideoFrame() call encoders synchronously. They are not WASAPI/DXGI callback APIs. Capture backends must enqueue frames to dedicated media-processing threads.

## Remaining integration
- Add concrete FFmpeg encoder and muxer factories.
- Verify callback rejection and shutdown behavior in each adapter.
- Add concurrent stop-vs-submit, flush-failure and sustained queue-pressure tests.
- Produce and decode a synthetic audio/video Matroska recording.
