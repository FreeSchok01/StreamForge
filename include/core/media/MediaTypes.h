#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace streamforge {

inline constexpr std::int64_t kNoTimestamp =
    std::numeric_limits<std::int64_t>::min();

struct MediaTimebase {
    std::int32_t numerator{1};
    std::int32_t denominator{1};

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return numerator > 0 && denominator > 0;
    }
};

enum class EncodedMediaType : std::uint8_t { Audio, Video };
enum class EncodedCodec : std::uint8_t { H264, HEVC, AV1, AAC, Opus };
enum class PixelFormat : std::uint8_t { Unknown, BGRA8, NV12, YUV420P };
enum class AudioSampleFormat : std::uint8_t {
    Unknown, Float32, Float32Planar, Signed16, Signed16Planar
};
enum class AudioChannelLayout : std::uint8_t {
    Unknown, Mono, Stereo, Surround51, Surround71
};

struct EncodedPacket {
    EncodedMediaType mediaType{EncodedMediaType::Video};
    std::int32_t streamIndex{-1};
    std::int64_t pts{kNoTimestamp};
    std::int64_t dts{kNoTimestamp};
    std::int64_t duration{0};
    MediaTimebase timebase{};
    bool keyFrame{false};
    std::shared_ptr<const std::vector<std::uint8_t>> data;

    [[nodiscard]] bool isValid() const noexcept {
        return streamIndex >= 0 && timebase.isValid() && data &&
               !data->empty() && duration >= 0;
    }
};

} // namespace streamforge
