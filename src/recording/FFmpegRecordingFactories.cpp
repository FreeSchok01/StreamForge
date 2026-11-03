#include "FFmpegRecordingFactories.h"

#include "FFmpegAudioEncoder.h"
#include "FFmpegMediaMuxer.h"
#include "FFmpegVideoEncoder.h"

#include <memory>
#include <utility>

namespace streamforge {

RecordingPipelineFactories makeFFmpegRecordingPipelineFactories() {
    RecordingPipelineFactories factories;
    factories.createAudioEncoder = [](
        const AudioEncoderConfig& config,
        EncodedPacketCallback callback,
        std::int32_t streamIndex) {
        return std::make_unique<FFmpegAudioEncoder>(
            config, std::move(callback), streamIndex);
    };
    factories.createVideoEncoder = [](
        const VideoEncoderConfig& config,
        EncodedPacketCallback callback,
        std::int32_t streamIndex) {
        return std::make_unique<FFmpegVideoEncoder>(
            config, std::move(callback), streamIndex);
    };
    factories.createMuxer = [] {
        return createFFmpegMediaMuxer();
    };
    return factories;
}

} // namespace streamforge
