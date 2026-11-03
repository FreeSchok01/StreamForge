#pragma once

#include "AudioDeviceInfo.h"

#include <string>
#include <vector>

namespace streamforge {

class IAudioDeviceEnumerator {
public:
    virtual ~IAudioDeviceEnumerator() = default;
    virtual bool enumerate(AudioDeviceFlow flow,
                           std::vector<AudioDeviceInfo>& devices,
                           std::string& error) const = 0;
    virtual bool getDefault(AudioDeviceFlow flow,
                            AudioDeviceInfo& device,
                            std::string& error) const = 0;
};

} // namespace streamforge
