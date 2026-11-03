#pragma once

#include "core/media/AudioFrame.h"
#include "core/media/MediaTypes.h"

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

struct AudioCaptureFormat {
    std::int32_t sampleRate{0};
    std::int32_t channels{0};
    AudioSampleFormat sampleFormat{AudioSampleFormat::Float32};
    AudioChannelLayout channelLayout{AudioChannelLayout::Unknown};

    [[nodiscard]] bool isValid() const noexcept {
        return sampleRate > 0 && channels > 0 &&
               sampleFormat == AudioSampleFormat::Float32 &&
               channelLayout != AudioChannelLayout::Unknown;
    }
};

using AudioCaptureCallback =
    std::function<bool(AudioFrame&& frame, std::string& error)>;

class IAudioCaptureSource {
public:
    virtual ~IAudioCaptureSource() = default;
    virtual bool queryFormat(const AudioCaptureConfig& config,
                             AudioCaptureFormat& format,
                             std::string& error) const = 0;
    virtual bool start(const AudioCaptureConfig& config,
                       AudioCaptureCallback callback,
                       std::string& error) = 0;
    virtual bool stop(std::string& error) = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
};

} // namespace streamforge
