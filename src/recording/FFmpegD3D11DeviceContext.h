#pragma once

#ifdef _WIN32

#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavutil/buffer.h>
}

namespace streamforge {

// Wraps the capture device in an FFmpeg D3D11VA device context. The returned
// reference shares the original D3D11 device and immediate context; it never
// creates a second device. The returned AVBufferRef is borrowed and remains
// valid until shutdown(), so callers must take their own av_buffer_ref().
class FFmpegD3D11DeviceContext final {
public:
    FFmpegD3D11DeviceContext() = default;
    ~FFmpegD3D11DeviceContext();

    FFmpegD3D11DeviceContext(const FFmpegD3D11DeviceContext&) = delete;
    FFmpegD3D11DeviceContext& operator=(const FFmpegD3D11DeviceContext&) = delete;
    FFmpegD3D11DeviceContext(FFmpegD3D11DeviceContext&&) = delete;
    FFmpegD3D11DeviceContext& operator=(FFmpegD3D11DeviceContext&&) = delete;

    bool initialize(void* d3d11Device,
                    void* immediateContext,
                    void* videoDevice,
                    void* videoContext,
                    std::shared_ptr<std::recursive_mutex> contextMutex,
                    std::string& error);

    // Borrowed reference. Use av_buffer_ref() when storing it elsewhere.
    [[nodiscard]] AVBufferRef* reference() const noexcept;

    void shutdown() noexcept;

private:
    AVBufferRef* m_deviceRef{nullptr};
};

} // namespace streamforge

#endif // _WIN32
