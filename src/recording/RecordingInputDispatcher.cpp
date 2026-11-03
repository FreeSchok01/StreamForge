#include "RecordingInputDispatcher.h"

#include <exception>
#include <utility>

namespace streamforge {

RecordingInputDispatcher::RecordingInputDispatcher(RecordingPipeline& pipeline)
    : m_pipeline(pipeline) {}

RecordingInputDispatcher::~RecordingInputDispatcher() {
    if (m_started) {
        std::string ignored;
        (void)stop(RecordingStopMode::Abort, ignored);
    }
}

bool RecordingInputDispatcher::start(std::string& error) {
    std::lock_guard lifecycle(m_lifecycleMutex);
    if (m_started ||
        m_state.load(std::memory_order_acquire) != RecordingInputDispatcherState::Idle) {
        error = "RecordingInputDispatcher ist bereits gestartet oder wurde beendet.";
        return false;
    }
    if (m_pipeline.snapshot().state != RecordingState::Recording) {
        error = "RecordingPipeline muss vor dem InputDispatcher gestartet werden.";
        return false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_abortRequested.store(false, std::memory_order_release);
    m_state.store(RecordingInputDispatcherState::Running, std::memory_order_release);

    try {
        m_audioThread = std::thread([this] { audioWorker(); });
        m_videoThread = std::thread([this] { videoWorker(); });
    } catch (const std::exception& ex) {
        m_accepting.store(false, std::memory_order_release);
        m_stopRequested.store(true, std::memory_order_release);
        notifyWorkers();
        if (m_audioThread.joinable()) m_audioThread.join();
        if (m_videoThread.joinable()) m_videoThread.join();
        m_state.store(RecordingInputDispatcherState::Failed, std::memory_order_release);
        error = std::string("Input-Worker konnten nicht gestartet werden: ") + ex.what();
        setFailure(Failure::ThreadStartFailure, error);
        std::string pipelineError;
        (void)m_pipeline.stop(RecordingStopMode::Abort, pipelineError);
        return false;
    }

    m_started = true;
    m_accepting.store(true, std::memory_order_release);
    return true;
}

bool RecordingInputDispatcher::enterSubmit(
    std::atomic<std::uint32_t>& active) noexcept {
    if (!m_accepting.load(std::memory_order_acquire)) return false;
    active.fetch_add(1, std::memory_order_acq_rel);
    if (!m_accepting.load(std::memory_order_acquire)) {
        leaveSubmit(active);
        return false;
    }
    return true;
}

void RecordingInputDispatcher::leaveSubmit(
    std::atomic<std::uint32_t>& active) noexcept {
    if (active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard lock(m_waitMutex);
        m_submittersIdle.notify_all();
    }
}

void RecordingInputDispatcher::setFailure(
    Failure failure, std::string message) noexcept {
    {
        std::lock_guard lock(m_errorMutex);
        if (m_failure.load(std::memory_order_relaxed) == Failure::None) {
            m_error = std::move(message);
            m_failure.store(failure, std::memory_order_release);
        }
    }
    m_accepting.store(false, std::memory_order_release);
    m_abortRequested.store(true, std::memory_order_release);
    m_stopRequested.store(true, std::memory_order_release);
    m_state.store(RecordingInputDispatcherState::Failed, std::memory_order_release);
    notifyWorkers();
}

void RecordingInputDispatcher::notifyWorkers() noexcept {
    m_workAvailable.notify_all();
}

bool RecordingInputDispatcher::trySubmitAudio(AudioFrame&& frame) noexcept {
    if (!enterSubmit(m_activeAudioSubmitters)) return false;
    struct Exit final {
        RecordingInputDispatcher* self;
        ~Exit() { self->leaveSubmit(self->m_activeAudioSubmitters); }
    } exit{this};

    if (!frame.isValid()) {
        setFailure(Failure::InvalidAudio, "Ungültiger Audioframe im Capture-Eingang.");
        return false;
    }
    if (!m_audioQueue.tryPush(std::move(frame))) {
        m_audioQueueOverflows.fetch_add(1, std::memory_order_relaxed);
        setFailure(Failure::AudioQueueOverflow,
                   "Audio-Verarbeitungswarteschlange ist voll; Aufnahme wird abgebrochen.");
        return false;
    }
    m_workAvailable.notify_one();
    return true;
}

bool RecordingInputDispatcher::trySubmitVideo(OwnedVideoFrame&& frame) noexcept {
    if (!enterSubmit(m_activeVideoSubmitters)) return false;
    struct Exit final {
        RecordingInputDispatcher* self;
        ~Exit() { self->leaveSubmit(self->m_activeVideoSubmitters); }
    } exit{this};

    if (!frame.isValid()) {
        setFailure(Failure::InvalidVideo, "Ungültiger Videoframe im Capture-Eingang.");
        return false;
    }
    if (!m_videoQueue.tryPush(std::move(frame))) {
        m_droppedVideoFrames.fetch_add(1, std::memory_order_relaxed);
        return false; // Explicit drop-newest policy for video.
    }
    m_workAvailable.notify_one();
    return true;
}

void RecordingInputDispatcher::audioWorker() {
    for (;;) {
        if (m_failure.load(std::memory_order_acquire) != Failure::None ||
            m_abortRequested.load(std::memory_order_acquire)) return;

        AudioFrame frame;
        if (m_audioQueue.tryPop(frame)) {
            std::string error;
            if (!m_pipeline.submitAudioFrame(frame, error)) {
                setFailure(Failure::AudioEncoderFailure,
                           error.empty() ? "Audioencoder-Verarbeitung fehlgeschlagen." : error);
                return;
            }
            continue;
        }

        if (m_stopRequested.load(std::memory_order_acquire)) return;
        std::unique_lock lock(m_waitMutex);
        m_workAvailable.wait(lock, [this] {
            return m_stopRequested.load(std::memory_order_acquire) ||
                   m_failure.load(std::memory_order_acquire) != Failure::None ||
                   m_abortRequested.load(std::memory_order_acquire) ||
                   !m_audioQueue.empty();
        });
    }
}

void RecordingInputDispatcher::videoWorker() {
    for (;;) {
        if (m_failure.load(std::memory_order_acquire) != Failure::None) return;

        OwnedVideoFrame frame;
        if (m_videoQueue.tryPop(frame)) {
            const VideoFrameInput view = frame.view();
            std::string error;
            if (!m_pipeline.submitVideoFrame(view, error)) {
                setFailure(Failure::VideoEncoderFailure,
                           error.empty() ? "Videoencoder-Verarbeitung fehlgeschlagen." : error);
                return;
            }
            continue;
        }

        if (m_stopRequested.load(std::memory_order_acquire)) return;
        std::unique_lock lock(m_waitMutex);
        m_workAvailable.wait(lock, [this] {
            return m_stopRequested.load(std::memory_order_acquire) ||
                   m_failure.load(std::memory_order_acquire) != Failure::None ||
                   m_abortRequested.load(std::memory_order_acquire) ||
                   !m_videoQueue.empty();
        });
    }
}

bool RecordingInputDispatcher::stop(RecordingStopMode mode, std::string& error) {
    std::lock_guard lifecycle(m_lifecycleMutex);
    if (!m_started) {
        const auto current = m_state.load(std::memory_order_acquire);
        if (current == RecordingInputDispatcherState::Failed) {
            std::lock_guard errorLock(m_errorMutex);
            error = m_error;
            return false;
        }
        return true;
    }

    m_state.store(RecordingInputDispatcherState::Stopping, std::memory_order_release);
    m_accepting.store(false, std::memory_order_release);
    {
        std::unique_lock lock(m_waitMutex);
        m_submittersIdle.wait(lock, [this] {
            return m_activeAudioSubmitters.load(std::memory_order_acquire) == 0 &&
                   m_activeVideoSubmitters.load(std::memory_order_acquire) == 0;
        });
    }

    if (mode == RecordingStopMode::Abort) {
        m_abortRequested.store(true, std::memory_order_release);
    }
    m_stopRequested.store(true, std::memory_order_release);
    notifyWorkers();
    if (m_audioThread.joinable()) m_audioThread.join();
    if (m_videoThread.joinable()) m_videoThread.join();

    m_audioQueue.clear();
    m_videoQueue.clear();
    m_started = false;

    const bool failed = m_failure.load(std::memory_order_acquire) != Failure::None;
    const RecordingStopMode pipelineMode =
        failed ? RecordingStopMode::Abort : mode;
    std::string pipelineError;
    const bool pipelineOk = m_pipeline.stop(pipelineMode, pipelineError);

    if (failed) {
        std::lock_guard lock(m_errorMutex);
        error = m_error;
        if (!pipelineError.empty()) error += " Pipeline: " + pipelineError;
        m_state.store(RecordingInputDispatcherState::Failed, std::memory_order_release);
        return false;
    }
    if (!pipelineOk) {
        error = pipelineError;
        m_state.store(RecordingInputDispatcherState::Failed, std::memory_order_release);
        return false;
    }

    m_state.store(RecordingInputDispatcherState::Stopped, std::memory_order_release);
    error.clear();
    return true;
}

RecordingInputDispatcherSnapshot RecordingInputDispatcher::snapshot() const {
    RecordingInputDispatcherSnapshot result;
    result.state = m_state.load(std::memory_order_acquire);
    result.audioQueueOverflows = m_audioQueueOverflows.load(std::memory_order_relaxed);
    result.droppedVideoFrames = m_droppedVideoFrames.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(m_errorMutex);
        result.error = m_error;
    }
    return result;
}

} // namespace streamforge
