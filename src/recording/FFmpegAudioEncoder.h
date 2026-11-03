#pragma once

#include "core/encoding/IAudioEncoder.h"

#include <memory>

namespace streamforge {

class FFmpegAudioEncoder final : public IAudioEncoder {
public:
    FFmpegAudioEncoder(AudioEncoderConfig config,
                       EncodedPacketCallback callback,
                       std::int32_t streamIndex);
    ~FFmpegAudioEncoder() override;

    FFmpegAudioEncoder(const FFmpegAudioEncoder&) = delete;
    FFmpegAudioEncoder& operator=(const FFmpegAudioEncoder&) = delete;
    FFmpegAudioEncoder(FFmpegAudioEncoder&&) = delete;
    FFmpegAudioEncoder& operator=(FFmpegAudioEncoder&&) = delete;

    bool initialize(std::string& error) override;
    bool encode(const AudioFrame& frame, std::string& error) override;
    bool flush(std::string& error) override;
    bool getStreamDescriptor(
        EncodedStreamDescriptor& descriptor, std::string& error) const override;
    void shutdown() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace streamforge
