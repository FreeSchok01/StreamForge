#pragma once

#include "core/media/VideoFrameInput.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace streamforge {

// Owns CPU fallback pixels and may additionally retain a native GPU resource.
struct OwnedVideoFrame {
    std::int32_t width{0};
    std::int32_t height{0};
    PixelFormat pixelFormat{PixelFormat::Unknown};
    std::array<std::int32_t, 4> linesize{};
    std::array<std::vector<std::uint8_t>, 4> planes{};
    std::int64_t pts{kNoTimestamp};
    std::int64_t timestampNs{kNoTimestamp};
    MediaTimebase timebase{};
    NativeVideoResourceType nativeResourceType{NativeVideoResourceType::None};
    void* nativeResource{nullptr};
    void* nativeDevice{nullptr};
    void* nativeContext{nullptr};
    std::shared_ptr<std::mutex> nativeContextMutex;
    std::shared_ptr<void> nativeResourceOwner;

    [[nodiscard]] bool isValid() const noexcept {
        if (width <= 0 || height <= 0 || !timebase.isValid()) return false;

        const bool nativeFormatSupported =
            pixelFormat == PixelFormat::BGRA8 ||
            (pixelFormat == PixelFormat::NV12 &&
             (width % 2) == 0 && (height % 2) == 0);
        const bool hasNativeResource =
            nativeResourceType == NativeVideoResourceType::D3D11Texture2D &&
            nativeFormatSupported && nativeResource && nativeDevice &&
            nativeContext && nativeContextMutex && nativeResourceOwner;
        const bool hasNoNativeResource =
            nativeResourceType == NativeVideoResourceType::None &&
            !nativeResource && !nativeDevice && !nativeContext &&
            !nativeContextMutex && !nativeResourceOwner;
        if (!hasNativeResource && !hasNoNativeResource) return false;

        const auto hasBytes = [](const std::vector<std::uint8_t>& bytes,
                                 std::int32_t stride, std::int32_t rows) {
            if (stride <= 0 || rows <= 0) return false;
            const auto required =
                static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(rows);
            return required <= bytes.size();
        };

        bool hasCpuPixels = false;
        switch (pixelFormat) {
        case PixelFormat::BGRA8:
            hasCpuPixels = width <= INT32_MAX / 4 &&
                hasBytes(planes[0], linesize[0], height) &&
                linesize[0] >= width * 4;
            break;
        case PixelFormat::NV12:
            hasCpuPixels = (width % 2) == 0 && (height % 2) == 0 &&
                linesize[0] >= width && linesize[1] >= width &&
                hasBytes(planes[0], linesize[0], height) &&
                hasBytes(planes[1], linesize[1], height / 2);
            break;
        case PixelFormat::YUV420P:
            hasCpuPixels = (width % 2) == 0 && (height % 2) == 0 &&
                linesize[0] >= width && linesize[1] >= width / 2 &&
                linesize[2] >= width / 2 &&
                hasBytes(planes[0], linesize[0], height) &&
                hasBytes(planes[1], linesize[1], height / 2) &&
                hasBytes(planes[2], linesize[2], height / 2);
            break;
        case PixelFormat::Unknown:
            break;
        }
        return hasCpuPixels || hasNativeResource;
    }

    [[nodiscard]] VideoFrameInput view() const noexcept {
        VideoFrameInput result;
        result.width = width;
        result.height = height;
        result.pixelFormat = pixelFormat;
        result.linesize = linesize;
        result.pts = pts;
        result.timestampNs = timestampNs;
        result.timebase = timebase;
        result.nativeResourceType = nativeResourceType;
        result.nativeResource = nativeResource;
        result.nativeDevice = nativeDevice;
        result.nativeContext = nativeContext;
        result.nativeContextMutex = nativeContextMutex;
        result.nativeResourceOwner = nativeResourceOwner;
        for (std::size_t index = 0; index < planes.size(); ++index) {
            result.data[index] = planes[index].empty() ? nullptr : planes[index].data();
        }
        return result;
    }
};

} // namespace streamforge
