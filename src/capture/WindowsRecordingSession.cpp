#include "WindowsRecordingSession.h"

#include "FFmpegRecordingFactories.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace streamforge {

WindowsRecordingSession::WindowsRecordingSession(RecordingPipelineFactories factories)
    : m_pipeline(std::move(factories)), m_dispatcher(m_pipeline) {}

WindowsRecordingSession::~WindowsRecordingSession() {
    if (m_started) {
        std::string ignored;
        (void)stop(RecordingStopMode::Abort, ignored);
    }
}

bool WindowsRecordingSession::start(
    const WindowsRecordingSessionConfig& config, std::string& error) {
    if (m_started) {
        error = "WindowsRecordingSession wurde bereits gestartet.";
        return false;
    }
    if (config.audioCapture.bufferDurationMs == 0 ||
        config.audioCapture.bufferDurationMs > 5000 ||
        config.videoCapture.acquireTimeoutMs == 0 ||
        config.videoCapture.acquireTimeoutMs > 1000) {
        error = "Windows-Capture-Konfiguration enthält ungültige Puffer- oder Timeoutwerte.";
        return false;
    }

    WindowsRecordingSessionConfig effective = config;
    AudioCaptureFormat audioFormat;
    if (!m_audioCapture.queryFormat(
            effective.audioCapture, audioFormat, error)) {
        return false;
    }
    effective.recording.audio.inputSampleRate = audioFormat.sampleRate;
    effective.recording.audio.inputChannels = audioFormat.channels;

    if (effective.recording.timestampOriginNs == kNoTimestamp) {
        const std::int64_t nowNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const std::int64_t safetyMarginNs =
            (static_cast<std::int64_t>(effective.audioCapture.bufferDurationMs) +
             50) * 1'000'000;
        effective.recording.timestampOriginNs = nowNs - safetyMarginNs;
    }

    if (!m_pipeline.start(effective.recording, error)) return false;
    if (!m_dispatcher.start(error)) {
        std::string ignored;
        (void)m_pipeline.stop(RecordingStopMode::Abort, ignored);
        return false;
    }

    if (!m_audioCapture.start(
            effective.audioCapture,
            [this](AudioFrame&& frame, std::string& callbackError) {
                if (m_dispatcher.trySubmitAudio(std::move(frame))) return true;
                callbackError = m_dispatcher.snapshot().error;
                if (callbackError.empty()) {
                    callbackError = "Audio-Dispatcher nimmt keine Frames mehr an.";
                }
                return false;
            },
            error)) {
        std::string ignored;
        (void)m_dispatcher.stop(RecordingStopMode::Abort, ignored);
        return false;
    }
    m_audioStarted = true;

    if (!m_videoCapture.start(
            effective.videoCapture,
            [this](OwnedVideoFrame&& frame, std::string& callbackError) {
                if (m_dispatcher.trySubmitVideo(std::move(frame))) return true;
                const auto state = m_dispatcher.snapshot();
                if (state.state == RecordingInputDispatcherState::Failed) {
                    callbackError = state.error;
                    return false;
                }
                // A full video queue intentionally drops the newest frame.
                return true;
            },
            error)) {
        std::string captureError;
        (void)m_audioCapture.stop(captureError);
        m_audioStarted = false;
        std::string ignored;
        (void)m_dispatcher.stop(RecordingStopMode::Abort, ignored);
        return false;
    }
    m_videoStarted = true;
    m_started = true;
    return true;
}

bool WindowsRecordingSession::stop(
    RecordingStopMode mode, std::string& error) {
    if (!m_started) {
        error.clear();
        return true;
    }

    std::string audioError;
    std::string videoError;
    bool captureOk = true;

    if (m_videoStarted) {
        if (!m_videoCapture.stop(videoError)) captureOk = false;
        m_videoStarted = false;
    }
    if (m_audioStarted) {
        if (!m_audioCapture.stop(audioError)) captureOk = false;
        m_audioStarted = false;
    }

    const RecordingStopMode effectiveMode =
        captureOk ? mode : RecordingStopMode::Abort;
    std::string dispatcherError;
    const bool dispatcherOk = m_dispatcher.stop(effectiveMode, dispatcherError);
    m_started = false;

    if (!captureOk || !dispatcherOk) {
        if (!videoError.empty()) error = "DXGI: " + videoError;
        if (!audioError.empty()) {
            if (!error.empty()) error += " | ";
            error += "WASAPI: " + audioError;
        }
        if (!dispatcherError.empty()) {
            if (!error.empty()) error += " | ";
            error += "Recording: " + dispatcherError;
        }
        return false;
    }

    error.clear();
    return true;
}

WindowsRecordingSessionSnapshot WindowsRecordingSession::snapshot() const {
    WindowsRecordingSessionSnapshot result;
    result.pipeline = m_pipeline.snapshot();
    result.audioCaptureRunning = m_audioCapture.isRunning();
    result.videoCaptureRunning = m_videoCapture.isRunning();
    result.dispatcherRunning =
        m_dispatcher.snapshot().state == RecordingInputDispatcherState::Running;

    const bool sessionStarted = m_started.load(std::memory_order_acquire);
    result.captureFailureDetected = sessionStarted &&
        (!result.audioCaptureRunning || !result.videoCaptureRunning ||
         !result.dispatcherRunning);
    if (!result.audioCaptureRunning) {
        result.captureError = m_audioCapture.lastError();
        if (result.captureError.empty() && sessionStarted) {
            result.captureError = "WASAPI-Audio-Capture wurde unerwartet beendet.";
        }
    }
    if (!result.videoCaptureRunning && sessionStarted) {
        const std::string videoError = m_videoCapture.lastError();
        if (!result.captureError.empty()) result.captureError += " | ";
        result.captureError += videoError.empty()
            ? "DXGI-Video-Capture wurde unerwartet beendet."
            : "DXGI: " + videoError;
    }
    if (!result.dispatcherRunning && sessionStarted) {
        if (!result.captureError.empty()) result.captureError += " | ";
        result.captureError += "Recording-Dispatcher wurde unerwartet beendet.";
    }
    return result;
}

bool WindowsRecordingSession::isRunning() const noexcept {
    return m_started.load(std::memory_order_acquire) &&
           m_audioCapture.isRunning() &&
           m_videoCapture.isRunning() && m_dispatcher.isRunning();
}

} // namespace streamforge
