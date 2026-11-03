#pragma once

#include "core/encoding/EncoderConfig.h"
#include "core/encoding/IAudioEncoder.h"
#include "core/encoding/IVideoEncoder.h"
#include "core/recording/IMediaMuxer.h"
#include "core/recording/PacketQueueConfig.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace streamforge {

enum class RecordingState : std::uint8_t {
    Idle, Initializing, Recording, Stopping, Finalizing,
    Completed, Aborted, Failed
};

enum class RecordingStopMode : std::uint8_t { Graceful, Abort };

struct RecordingPipelineConfig {
    AudioEncoderConfig audio;
    VideoEncoderConfig video;
    RecordingOutputConfig output;
    PacketQueueConfig packetQueue;
    std::int64_t timestampOriginNs{kNoTimestamp};
    std::int32_t audioStreamIndex{0};
    std::int32_t videoStreamIndex{1};
};

struct RecordingPipelineFactories {
    using AudioEncoderFactory = std::function<std::unique_ptr<IAudioEncoder>(
        const AudioEncoderConfig&, EncodedPacketCallback, std::int32_t)>;
    using VideoEncoderFactory = std::function<std::unique_ptr<IVideoEncoder>(
        const VideoEncoderConfig&, EncodedPacketCallback, std::int32_t)>;
    using MuxerFactory = std::function<std::unique_ptr<IMediaMuxer>()>;

    AudioEncoderFactory createAudioEncoder;
    VideoEncoderFactory createVideoEncoder;
    MuxerFactory createMuxer;
};

struct RecordingPipelineSnapshot {
    RecordingState state{RecordingState::Idle};
    std::string error;
    bool acceptsInput{false};
};

// One instance represents one recording session and is single-use.
class RecordingPipeline final {
public:
    explicit RecordingPipeline(RecordingPipelineFactories factories);
    ~RecordingPipeline();

    RecordingPipeline(const RecordingPipeline&) = delete;
    RecordingPipeline& operator=(const RecordingPipeline&) = delete;
    RecordingPipeline(RecordingPipeline&&) = delete;
    RecordingPipeline& operator=(RecordingPipeline&&) = delete;

    bool start(const RecordingPipelineConfig& config, std::string& error);

    // Synchronous encoder calls. Use dedicated media-processing threads,
    // never OS capture callbacks such as WASAPI or DXGI callbacks.
    bool submitAudioFrame(const AudioFrame& frame, std::string& error);
    bool submitVideoFrame(const VideoFrameInput& frame, std::string& error);

    bool stop(RecordingStopMode mode, std::string& error);
    [[nodiscard]] RecordingPipelineSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace streamforge
