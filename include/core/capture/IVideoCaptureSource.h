#pragma once

#include "core/media/OwnedVideoFrame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace streamforge {

struct VideoCaptureConfig {
    std::uint32_t adapterIndex{0};
    std::uint32_t outputIndex{0};
    std::uint32_t acquireTimeoutMs{100};
    // Disable CPU staging readback when every downstream consumer is GPU-native.
    // The retained D3D11 texture is still produced and leased to the callback.
    bool includeCpuReadback{true};
    // Retain an independent D3D11 texture for downstream native GPU consumers.
    bool includeNativeResource{true};
};

using VideoCaptureCallback =
    std::function<bool(OwnedVideoFrame&& frame, std::string& error)>;

class IVideoCaptureSource {
public:
    virtual ~IVideoCaptureSource() = default;
    virtual bool start(const VideoCaptureConfig& config,
                       VideoCaptureCallback callback,
                       std::string& error) = 0;
    virtual bool stop(std::string& error) = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
};

} // namespace streamforge
