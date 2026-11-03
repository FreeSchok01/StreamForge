#pragma once

#include "core/capture/IAudioCaptureSource.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace streamforge {

class WasapiAudioCaptureSource final : public IAudioCaptureSource {
public:
    WasapiAudioCaptureSource();
    ~WasapiAudioCaptureSource() override;

    WasapiAudioCaptureSource(const WasapiAudioCaptureSource&) = delete;
    WasapiAudioCaptureSource& operator=(const WasapiAudioCaptureSource&) = delete;
    WasapiAudioCaptureSource(WasapiAudioCaptureSource&&) = delete;
    WasapiAudioCaptureSource& operator=(WasapiAudioCaptureSource&&) = delete;

    bool queryFormat(const AudioCaptureConfig& config,
                     AudioCaptureFormat& format,
                     std::string& error) const override;
    bool start(const AudioCaptureConfig& config,
               AudioCaptureCallback callback,
               std::string& error) override;
    bool stop(std::string& error) override;
    [[nodiscard]] bool isRunning() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace streamforge
