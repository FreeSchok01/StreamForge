# Windows Capture Hardware Validation

This checklist covers validation that GitHub Actions cannot perform because it requires real Windows audio endpoints, displays, and GPU drivers.

## Test environment record

Record the following for every test run:

- Windows edition, version, and build (Windows 10 22H2 or Windows 11)
- CPU model and core count
- GPU model, driver version, and adapter index
- Display model, resolution, refresh rate, scaling, and output index
- Audio endpoint names, endpoint IDs, sample rates, channel counts, and default-device status
- StreamForge commit SHA and build configuration
- FFmpeg version and available encoders
- Output container, codec settings, and recording duration

Never include stream keys, access tokens, or other credentials in logs or test artifacts.

## WASAPI capture

- [ ] Enumerate active capture and render endpoints; verify friendly names and stable endpoint IDs.
- [ ] Query the default microphone format and start/stop capture repeatedly.
- [ ] Query a selected microphone format and capture at least 10 minutes.
- [ ] Capture render-loopback audio from the default output endpoint.
- [ ] Verify Float32 output, channel count, frame count, and monotonic timestamps.
- [ ] Exercise mono, stereo, and multichannel devices where available.
- [ ] Verify silent packets are represented as zero-valued samples.
- [ ] Unplug or disable the selected endpoint during capture; verify a clear worker error and clean shutdown.
- [ ] Change the default endpoint while capturing; document current behavior (automatic recovery is not implemented yet).
- [ ] Confirm that stop drains queued audio and that abort does not write stale queued frames.

## DXGI Desktop Duplication

- [ ] Capture each connected display independently.
- [ ] Verify frame dimensions, BGRA8 layout, row stride, and monotonic timestamps.
- [ ] Test 1080p60 and at least one higher-resolution or high-refresh display.
- [ ] Change display resolution and orientation before starting a new capture.
- [ ] Lock/unlock the workstation and test display sleep/wake.
- [ ] Disconnect/reconnect a display during capture; verify DXGI retries duplication recovery for up to 20 attempts (250 ms intervals), resumes when the same output becomes available, and reports a clear error if recovery fails.
- [ ] Test integrated and discrete GPU adapters where available.
- [ ] Measure CPU utilization and memory bandwidth for the staging-texture readback path.
- [ ] Confirm stop interrupts the acquisition loop within the configured acquire timeout.

## End-to-end recording

- [ ] Record microphone + desktop video to Matroska for 10 minutes.
- [ ] Record microphone + desktop video to MP4 for 10 minutes.
- [ ] Verify both audio and video streams are present and decodable.
- [ ] Inspect packet DTS monotonicity and A/V synchronization at the beginning, middle, and end.
- [ ] Repeat with a busy desktop and continuous audio playback.
- [ ] Fill the bounded video queue deliberately; verify newest-frame drops are counted and recording continues.
- [ ] Exercise audio queue saturation in a controlled test; verify the session fails visibly rather than silently losing audio.
- [ ] Stop gracefully and verify the container trailer is written.
- [ ] Abort and verify all capture workers, dispatcher workers, and file handles are released.
- [ ] Repeat start/stop cycles at least 20 times.

## Evidence and acceptance

For each failed check, attach the relevant sanitized log excerpt and note the exact reproduction steps. A test passes only when the recording can be reopened and decoded, both streams are present, timestamps remain valid, and all workers terminate cleanly.

This checklist is a manual validation plan, not evidence that the listed hardware tests have already passed.

## Known current limitations

- WASAPI endpoint hotplug/default-device recovery is not implemented.
- DXGI retries `DXGI_ERROR_ACCESS_LOST` recovery against the existing adapter/output for up to approximately five seconds; adapter/output re-enumeration is not yet implemented, so topology changes that invalidate those objects may still require a new session.
- DXGI currently copies frames through a CPU-readable staging texture; GPU-native frame handoff is future work.
- Camera capture, cursor composition, and display rotation handling are not yet complete.
- Hardware encoder adapters are not yet implemented.
