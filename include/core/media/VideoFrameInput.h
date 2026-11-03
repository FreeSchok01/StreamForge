#pragma once

#include "MediaTypes.h"

#include <array>
#include <cstdint>
#include <memory>

namespace streamforge {

// CPU pixel pointers are non-owning and remain valid until encode() returns.
// Native resources carry a shared lifetime lease for asynchronous consumers.
// For D3D11, nativeResource is ID3D11Texture2D* and nativeDevice is its
// ID3D11Device*; both remain valid while nativeResourceOwner is retained.
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
    void* nativeDevice{nullptr};
    std::shared_ptr<void> nativeResourceOwner;

    [[nodiscard]] bool isValid() const noexcept {
        if (width <= 0 || height <= 0 || !timebase.isValid()) return false;

        const bool hasNativeResource =
            nativeResourceType == NativeVideoResourceType::D3D11Texture2D &&
            nativeResource && nativeDevice && nativeResourceOwner;
        const bool hasNoNativeResource =
            nativeResourceType == NativeVideoResourceType::None &&
            !nativeResource && !nativeDevice && !nativeResourceOwner;
        if (!hasNativeResource && !hasNoNativeResource) return false;

        bool hasCpuPixels = false;
        switch (pixelFormat) {
        case PixelFormat::BGRA8:
            hasCpuPixels = data[0] && width <= INT32_MAX / 4 &&
                linesize[0] >= width * 4;
            break;
        case PixelFormat::NV12:
            hasCpuPixels = (width % 2) == 0 && (height % 2) == 0 &&
                data[0] && data[1] && linesize[0] >= width &&
                linesize[1] >= width;
            break;
        case PixelFormat::YUV420P:
            hasCpuPixels = (width % 2) == 0 && (height % 2) == 0 &&
                data[0] && data[1] && data[2] &&
                linesize[0] >= width && linesize[1] >= width / 2 &&
                linesize[2] >= width / 2;
            break;
        case PixelFormat::Unknown:
            break;
        }
        return hasCpuPixels || hasNativeResource;
    }
};

} // namespace streamforge
