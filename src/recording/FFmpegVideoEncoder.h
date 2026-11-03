#pragma once

#include "core/encoding/IVideoEncoder.h"

#include <memory>

namespace streamforge {

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
