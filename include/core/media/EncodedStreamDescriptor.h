#pragma once

#include "MediaTypes.h"

#include <cstdint>
#include <vector>

namespace streamforge {

struct EncodedStreamDescriptor {
    std::int32_t streamIndex{-1};
    EncodedMediaType mediaType{EncodedMediaType::Video};
    EncodedCodec codec{EncodedCodec::H264};
    MediaTimebase timebase{};

    std::int32_t width{0};
    std::int32_t height{0};
    PixelFormat pixelFormat{PixelFormat::Unknown};
    std::int32_t frameRateNumerator{0};
    std::int32_t frameRateDenominator{1};

    std::int32_t sampleRate{0};
    std::int32_t channels{0};
    AudioChannelLayout channelLayout{AudioChannelLayout::Unknown};
    AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};

    std::int32_t bitrate{0};
    std::int32_t profile{-1};
    std::int32_t level{-1};
    std::vector<std::uint8_t> codecExtradata;

    [[nodiscard]] bool isValid() const noexcept {
        if (streamIndex < 0 || !timebase.isValid() || bitrate < 0) return false;
        if (mediaType == EncodedMediaType::Video) {
            const bool codecOk = codec == EncodedCodec::H264 ||
                                 codec == EncodedCodec::HEVC ||
                                 codec == EncodedCodec::AV1;
            return codecOk && width > 0 && height > 0 &&
                   frameRateNumerator > 0 && frameRateDenominator > 0 &&
                   pixelFormat != PixelFormat::Unknown;
        }
        const bool codecOk = codec == EncodedCodec::AAC ||
                             codec == EncodedCodec::Opus;
        return codecOk && sampleRate > 0 && channels > 0 &&
               channelLayout != AudioChannelLayout::Unknown &&
               sampleFormat != AudioSampleFormat::Unknown;
    }
};

} // namespace streamforge
