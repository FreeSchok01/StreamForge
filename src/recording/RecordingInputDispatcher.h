#pragma once

#include "RecordingPipeline.h"
#include "core/concurrency/SpscRingBuffer.h"
#include "core/media/OwnedVideoFrame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace streamforge {

enum class RecordingInputDispatcherState : std::uint8_t {
    Idle, Running, Stopping, Stopped, Failed
};

struct RecordingInputDispatcherSnapshot {
    RecordingInputDispatcherState state{RecordingInputDispatcherState::Idle};
    std::uint64_t audioQueueOverflows{0};
    std::uint64_t droppedVideoFrames{0};
    std::string error;
};

// One producer thread per input queue is required (e.g. one WASAPI producer
// and one DXGI producer). Capture callbacks only move frames into SPSC queues.
class RecordingInputDispatcher final {
public:
    explicit RecordingInputDispatcher(RecordingPipeline& pipeline);
    ~RecordingInputDispatcher();

    RecordingInputDispatcher(const RecordingInputDispatcher&) = delete;
    RecordingInputDispatcher& operator=(const RecordingInputDispatcher&) = delete;
    RecordingInputDispatcher(RecordingInputDispatcher&&) = delete;
    RecordingInputDispatcher& operator=(RecordingInputDispatcher&&) = delete;

    bool start(std::string& error);
    [[nodiscard]] bool trySubmitAudio(AudioFrame&& frame) noexcept;
    [[nodiscard]] bool trySubmitVideo(OwnedVideoFrame&& frame) noexcept;
    bool stop(RecordingStopMode mode, std::string& error);

    [[nodiscard]] RecordingInputDispatcherSnapshot snapshot() const;

private:
    enum class Failure : std::uint8_t {
        None, InvalidAudio, AudioQueueOverflow, AudioEncoderFailure,
        InvalidVideo, VideoEncoderFailure, ThreadStartFailure
    };

    static constexpr std::size_t kAudioQueueCapacity = 128;
    static constexpr std::size_t kVideoQueueCapacity = 32;

    bool enterSubmit(std::atomic<std::uint32_t>& active) noexcept;
    void leaveSubmit(std::atomic<std::uint32_t>& active) noexcept;
    void setFailure(Failure failure, std::string message) noexcept;
    void audioWorker();
    void videoWorker();
    void notifyWorkers() noexcept;

    RecordingPipeline& m_pipeline;
    SpscRingBuffer<AudioFrame, kAudioQueueCapacity> m_audioQueue;
    SpscRingBuffer<OwnedVideoFrame, kVideoQueueCapacity> m_videoQueue;

    std::atomic<RecordingInputDispatcherState> m_state{
        RecordingInputDispatcherState::Idle};
    std::atomic<bool> m_accepting{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<Failure> m_failure{Failure::None};
    std::atomic<std::uint32_t> m_activeAudioSubmitters{0};
    std::atomic<std::uint32_t> m_activeVideoSubmitters{0};
    std::atomic<std::uint64_t> m_audioQueueOverflows{0};
    std::atomic<std::uint64_t> m_droppedVideoFrames{0};

    mutable std::mutex m_errorMutex;
    std::string m_error;
    std::mutex m_waitMutex;
    std::condition_variable m_submittersIdle;
    std::condition_variable m_workAvailable;
    std::thread m_audioThread;
    std::thread m_videoThread;
    std::mutex m_lifecycleMutex;
    bool m_started{false};
};

} // namespace streamforge
