#pragma once

#include "MediaTypes.h"

#include <array>
#include <cstdint>
#include <memory>

namespace streamforge {

// Non-owning view. Pointers remain valid until encode() returns.
struct VideoFrameInput {
    std::array<const std::uint8_t*, 4> data{};
    std::array<std::int32_t, 4> linesize{};
    std::int32_t width{0};
    std::int32_t height{0};
    PixelFormat pixelFormat{PixelFormat::Unknown};
    std::int64_t pts{kNoTimestamp};
    std::int64_t timestampNs{kNoTimestamp};
    MediaTimebase timebase{};
    NativeVideoResourceType nativeResourceType{NativeVideoResourceType::None};
    void* nativeResource{nullptr};
    std::shared_ptr<void> nativeResourceOwner;

    [[nodiscard]] bool isValid() const noexcept {
        if (!data[0] || width <= 0 || height <= 0 || linesize[0] <= 0 ||
            pixelFormat == PixelFormat::Unknown || !timebase.isValid()) {
            return false;
        }
        switch (pixelFormat) {
        case PixelFormat::BGRA8:
            return width <= INT32_MAX / 4 && linesize[0] >= width * 4;
        case PixelFormat::NV12:
            return (width % 2) == 0 && (height % 2) == 0 &&
                   data[1] && linesize[0] >= width && linesize[1] >= width;
        case PixelFormat::YUV420P:
            return (width % 2) == 0 && (height % 2) == 0 &&
                   data[1] && data[2] && linesize[0] >= width &&
                   linesize[1] >= width / 2 && linesize[2] >= width / 2;
        default:
            return false;
        }
    }
};

} // namespace streamforge
