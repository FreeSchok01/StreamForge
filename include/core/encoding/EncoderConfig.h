#pragma once

#include <cstdint>

namespace streamforge {

enum class VideoCodec : std::uint8_t { H264, HEVC, AV1 };
enum class AudioCodec : std::uint8_t { AAC, Opus };
enum class VideoRateControl : std::uint8_t { CBR, VBR, CQP };
enum class VideoEncoderBackend : std::uint8_t { Software, NVENC, AMF, OneVPL };

struct VideoEncoderConfig {
    VideoCodec codec{VideoCodec::H264};
    VideoEncoderBackend backend{VideoEncoderBackend::Software};
    VideoRateControl rateControl{VideoRateControl::CBR};
    std::int32_t width{1920};
    std::int32_t height{1080};
    std::int32_t fpsNumerator{60};
    std::int32_t fpsDenominator{1};
    std::int32_t bitrate{6'000'000};
    std::int32_t maxBitrate{6'000'000};
    std::int32_t keyframeInterval{120};
    bool lowLatency{true};
    bool allowBFrames{false};
};

struct AudioEncoderConfig {
    AudioCodec codec{AudioCodec::AAC};
    std::int32_t inputSampleRate{48000};
    std::int32_t inputChannels{2};
    std::int32_t outputSampleRate{48000};
    std::int32_t outputChannels{2};
    std::int32_t bitrate{192000};
};

} // namespace streamforge
