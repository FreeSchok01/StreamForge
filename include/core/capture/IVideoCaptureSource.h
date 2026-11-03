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
