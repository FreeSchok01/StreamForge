#pragma once

#include "core/capture/IAudioDeviceEnumerator.h"

namespace streamforge {

class WasapiDeviceEnumerator final : public IAudioDeviceEnumerator {
public:
    bool enumerate(AudioDeviceFlow flow,
                   std::vector<AudioDeviceInfo>& devices,
                   std::string& error) const override;
    bool getDefault(AudioDeviceFlow flow,
                    AudioDeviceInfo& device,
                    std::string& error) const override;
};

} // namespace streamforge
