#pragma once

#ifdef _WIN32

#include "core/media/OwnedVideoFrame.h"

#include <cstdint>
#include <memory>
#include <string>

namespace streamforge {

// Converts a retained D3D11 BGRA8 frame to a native NV12 texture using the
// device's video processor. The input frame's shared context mutex serializes
// VideoProcessorBlt against DXGI capture copies and other GPU consumers.
class D3D11BgraToNv12Converter final {
public:
    D3D11BgraToNv12Converter();
    ~D3D11BgraToNv12Converter();

    D3D11BgraToNv12Converter(const D3D11BgraToNv12Converter&) = delete;
    D3D11BgraToNv12Converter& operator=(const D3D11BgraToNv12Converter&) = delete;
    D3D11BgraToNv12Converter(D3D11BgraToNv12Converter&&) = delete;
    D3D11BgraToNv12Converter& operator=(D3D11BgraToNv12Converter&&) = delete;

    bool initialize(const VideoFrameInput& firstFrame,
                    std::uint32_t fpsNumerator,
                    std::uint32_t fpsDenominator,
                    std::string& error);

    bool convert(const VideoFrameInput& input,
                 OwnedVideoFrame& output,
                 std::string& error);

    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace streamforge

#endif // _WIN32
