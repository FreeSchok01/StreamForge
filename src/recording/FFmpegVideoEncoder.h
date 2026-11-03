#pragma once

#include "core/encoding/IVideoEncoder.h"

#include <memory>
#include <string>
#include <vector>

namespace streamforge {

struct FFmpegVideoEncoderCapability {
    VideoEncoderBackend backend{VideoEncoderBackend::Software};
    VideoCodec codec{VideoCodec::H264};
    std::string encoderName;
    bool availableInBuild{false};
    bool supportsD3D11Frames{false};
    bool supportsCudaFrames{false};
};

class FFmpegVideoEncoder final : public IVideoEncoder {
public:
    FFmpegVideoEncoder(VideoEncoderConfig config,
                       EncodedPacketCallback callback,
                       std::int32_t streamIndex);
    ~FFmpegVideoEncoder() override;

    FFmpegVideoEncoder(const FFmpegVideoEncoder&) = delete;
    FFmpegVideoEncoder& operator=(const FFmpegVideoEncoder&) = delete;
    FFmpegVideoEncoder(FFmpegVideoEncoder&&) = delete;
    FFmpegVideoEncoder& operator=(FFmpegVideoEncoder&&) = delete;

    // Reports encoders compiled into the linked FFmpeg build. This does not
    // probe a physical GPU, driver state, or runtime codec support.
    static std::vector<FFmpegVideoEncoderCapability> queryCapabilities();

    bool initialize(std::string& error) override;
    bool encode(const VideoFrameInput& frame, std::string& error) override;
    bool flush(std::string& error) override;
    bool getStreamDescriptor(
        EncodedStreamDescriptor& descriptor, std::string& error) const override;
    void shutdown() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace streamforge
