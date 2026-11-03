#pragma once

#ifdef _WIN32

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace streamforge {

struct D3D11VideoProcessorCapabilities {
    bool videoProcessorAvailable{false};
    bool bgra8InputSupported{false};
    bool nv12OutputSupported{false};

    [[nodiscard]] bool supportsBgraToNv12() const noexcept {
        return videoProcessorAvailable &&
               bgra8InputSupported && nv12OutputSupported;
    }
};

// Probes the D3D11 video processor on the supplied device. This checks format
// support only; it does not create conversion views or perform a conversion.
bool queryD3D11BgraToNv12Capabilities(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fpsNumerator,
    std::uint32_t fpsDenominator,
    D3D11VideoProcessorCapabilities& capabilities,
    std::string& error);

} // namespace streamforge

#endif // _WIN32
