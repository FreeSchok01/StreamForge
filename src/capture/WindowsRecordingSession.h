#pragma once

#include "RecordingInputDispatcher.h"
#include "RecordingPipeline.h"
#include "DxgiDesktopCaptureSource.h"
#include "WasapiAudioCaptureSource.h"

#include <atomic>
#include <string>

namespace streamforge {

struct WindowsRecordingSessionConfig {
    RecordingPipelineConfig recording;
    AudioCaptureConfig audioCapture;
    VideoCaptureConfig videoCapture;
};

struct WindowsRecordingSessionSnapshot {
    RecordingPipelineSnapshot pipeline;
    bool audioCaptureRunning{false};
    bool videoCaptureRunning{false};
    bool dispatcherRunning{false};
    bool captureFailureDetected{false};
    std::string captureError;
};

// Coordinates the real Windows capture -> dispatcher -> encoder -> muxer path.
// Capture sources are stopped before the dispatcher drains/finalizes the pipeline.
class WindowsRecordingSession final {
public:
    explicit WindowsRecordingSession(RecordingPipelineFactories factories);
    ~WindowsRecordingSession();

    WindowsRecordingSession(const WindowsRecordingSession&) = delete;
    WindowsRecordingSession& operator=(const WindowsRecordingSession&) = delete;
    WindowsRecordingSession(WindowsRecordingSession&&) = delete;
    WindowsRecordingSession& operator=(WindowsRecordingSession&&) = delete;

    bool start(const WindowsRecordingSessionConfig& config, std::string& error);
    bool stop(RecordingStopMode mode, std::string& error);

    [[nodiscard]] WindowsRecordingSessionSnapshot snapshot() const;
    [[nodiscard]] bool isRunning() const noexcept;

private:
    RecordingPipeline m_pipeline;
    RecordingInputDispatcher m_dispatcher;
    WasapiAudioCaptureSource m_audioCapture;
    DxgiDesktopCaptureSource m_videoCapture;
    bool m_audioStarted{false};
    bool m_videoStarted{false};
    std::atomic<bool> m_started{false};
};

} // namespace streamforge
