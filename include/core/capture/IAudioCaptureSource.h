#pragma once

#include "core/media/AudioFrame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace streamforge {

struct AudioCaptureConfig {
    // Empty selects the Windows default endpoint for the chosen capture mode.
    std::wstring deviceId;
    bool loopback{false};
    std::uint32_t bufferDurationMs{100};
};

using AudioCaptureCallback =
    std::function<bool(AudioFrame&& frame, std::string& error)>;

class IAudioCaptureSource {
public:
    virtual ~IAudioCaptureSource() = default;
    virtual bool start(const AudioCaptureConfig& config,
                       AudioCaptureCallback callback,
                       std::string& error) = 0;
    virtual bool stop(std::string& error) = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
};

} // namespace streamforge
