#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FFmpegD3D11DeviceContext.h"

#ifdef _WIN32

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <exception>
#include <new>
#include <utility>

namespace streamforge {
namespace {

using Microsoft::WRL::ComPtr;

struct D3D11LockOwner final {
    explicit D3D11LockOwner(
        std::shared_ptr<std::recursive_mutex> value) noexcept
        : mutex(std::move(value)) {}

    std::shared_ptr<std::recursive_mutex> mutex;
};

void lockD3D11Context(void* opaque) noexcept {
    auto* mutex = static_cast<std::recursive_mutex*>(opaque);
    if (!mutex) std::terminate();
    try {
        mutex->lock();
    } catch (...) {
        // FFmpeg's lock callback has no error return. Continuing without the
        // shared immediate-context lock would violate the D3D11 threading contract.
        std::terminate();
    }
}

void unlockD3D11Context(void* opaque) noexcept {
    auto* mutex = static_cast<std::recursive_mutex*>(opaque);
    if (!mutex) std::terminate();
    try {
        mutex->unlock();
    } catch (...) {
        std::terminate();
    }
}

void freeD3D11DeviceOwner(AVHWDeviceContext* context) {
    if (!context) return;
    delete static_cast<D3D11LockOwner*>(context->user_opaque);
    context->user_opaque = nullptr;
}

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0) {
        return "Unbekannter FFmpeg-Fehler (" + std::to_string(code) + ")";
    }
    return buffer;
}

} // namespace

FFmpegD3D11DeviceContext::~FFmpegD3D11DeviceContext() {
    shutdown();
}

bool FFmpegD3D11DeviceContext::initialize(
    void* rawDevice,
    void* rawImmediateContext,
    void* rawVideoDevice,
    void* rawVideoContext,
    std::shared_ptr<std::recursive_mutex> contextMutex,
    std::string& error) {
    if (m_deviceRef) {
        error = "FFmpeg-D3D11VA-Gerätekontext wurde bereits initialisiert.";
        return false;
    }
    if (!rawDevice || !rawImmediateContext || !rawVideoDevice ||
        !rawVideoContext || !contextMutex) {
        error = "FFmpeg-D3D11VA benötigt Device, Immediate Context, Video-Interfaces und den gemeinsamen rekursiven Mutex.";
        return false;
    }

    auto* device = static_cast<ID3D11Device*>(rawDevice);
    auto* immediateContext =
        static_cast<ID3D11DeviceContext*>(rawImmediateContext);
    auto* videoDevice = static_cast<ID3D11VideoDevice*>(rawVideoDevice);
    auto* videoContext = static_cast<ID3D11VideoContext*>(rawVideoContext);

    if (immediateContext->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "FFmpeg-D3D11VA benötigt den Immediate Context.";
        return false;
    }

    ComPtr<ID3D11Device> contextDevice;
    immediateContext->GetDevice(&contextDevice);
    if (contextDevice.Get() != device) {
        error = "D3D11-Device und Immediate Context gehören nicht zusammen.";
        return false;
    }

    ComPtr<ID3D11VideoDevice> verifiedVideoDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&verifiedVideoDevice));
    if (FAILED(hr) || verifiedVideoDevice.Get() != videoDevice) {
        error = "ID3D11VideoDevice gehört nicht zum übergebenen D3D11-Gerät.";
        return false;
    }

    ComPtr<ID3D11VideoContext> verifiedVideoContext;
    hr = immediateContext->QueryInterface(IID_PPV_ARGS(&verifiedVideoContext));
    if (FAILED(hr) || verifiedVideoContext.Get() != videoContext) {
        error = "ID3D11VideoContext gehört nicht zum übergebenen Immediate Context.";
        return false;
    }

    AVBufferRef* deviceRef =
        av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!deviceRef) {
        error = "FFmpeg konnte keinen D3D11VA-Gerätekontext reservieren.";
        return false;
    }

    auto* owner = new (std::nothrow) D3D11LockOwner(std::move(contextMutex));
    if (!owner) {
        av_buffer_unref(&deviceRef);
        error = "Gemeinsamer D3D11-Context-Lock konnte nicht an FFmpeg gebunden werden.";
        return false;
    }

    auto* deviceContext =
        reinterpret_cast<AVHWDeviceContext*>(deviceRef->data);
    auto* d3d11Context =
        static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);

    // FFmpeg's D3D11VA uninitializer releases every populated COM interface.
    // Give it an independent reference for each field before assigning it.
    device->AddRef();
    immediateContext->AddRef();
    videoDevice->AddRef();
    videoContext->AddRef();

    d3d11Context->device = device;
    d3d11Context->device_context = immediateContext;
    d3d11Context->video_device = videoDevice;
    d3d11Context->video_context = videoContext;
    d3d11Context->lock = &lockD3D11Context;
    d3d11Context->unlock = &unlockD3D11Context;
    d3d11Context->lock_ctx = owner->mutex.get();

    deviceContext->user_opaque = owner;
    deviceContext->free = &freeD3D11DeviceOwner;

    const int result = av_hwdevice_ctx_init(deviceRef);
    if (result < 0) {
        av_buffer_unref(&deviceRef);
        error = "FFmpeg konnte den vorhandenen D3D11-Gerätekontext nicht initialisieren: " +
                ffmpegError(result);
        return false;
    }

    m_deviceRef = deviceRef;
    error.clear();
    return true;
}

AVBufferRef* FFmpegD3D11DeviceContext::reference() const noexcept {
    return m_deviceRef;
}

void FFmpegD3D11DeviceContext::shutdown() noexcept {
    av_buffer_unref(&m_deviceRef);
}

} // namespace streamforge

#endif // _WIN32
