#pragma once

#include <string>

namespace streamforge {

enum class AudioDeviceFlow { Capture, Render };

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    AudioDeviceFlow flow{AudioDeviceFlow::Capture};
    bool isDefault{false};
};

} // namespace streamforge
