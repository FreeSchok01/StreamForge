#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "D3D11VideoProcessorCapabilities.h"

#ifdef _WIN32

#include <wrl/client.h>


namespace streamforge {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresultText(HRESULT result) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string output = "HRESULT 0x00000000";
    unsigned long value = static_cast<unsigned long>(result);
    for (int index = 7; index >= 0; --index) {
        output[static_cast<std::size_t>(index) + 10U] = digits[value & 0x0FU];
        value >>= 4U;
    }
    return output;
}

} // namespace

bool queryD3D11BgraToNv12Capabilities(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fpsNumerator,
    std::uint32_t fpsDenominator,
    D3D11VideoProcessorCapabilities& capabilities,
    std::string& error) {
    capabilities = {};
    if (!device || width == 0 || height == 0 ||
        fpsNumerator == 0 || fpsDenominator == 0) {
        error = "D3D11-VideoProcessor benötigt ein gültiges Gerät, Bildmaß und Framerate.";
        return false;
    }

    ComPtr<ID3D11VideoDevice> videoDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&videoDevice));
    if (FAILED(hr)) {
        error = "D3D11-Gerät stellt ID3D11VideoDevice nicht bereit: " +
                hresultText(hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
    description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    description.InputFrameRate = {
        static_cast<UINT>(fpsNumerator), static_cast<UINT>(fpsDenominator)};
    description.InputWidth = static_cast<UINT>(width);
    description.InputHeight = static_cast<UINT>(height);
    description.OutputFrameRate = description.InputFrameRate;
    description.OutputWidth = description.InputWidth;
    description.OutputHeight = description.InputHeight;
    description.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    hr = videoDevice->CreateVideoProcessorEnumerator(
        &description, &enumerator);
    if (FAILED(hr)) {
        error = "D3D11-VideoProcessor-Enumerator konnte nicht erstellt werden: " +
                hresultText(hr);
        return false;
    }

    UINT bgraSupport = 0;
    hr = enumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, &bgraSupport);
    if (FAILED(hr)) {
        error = "D3D11 konnte BGRA8-VideoProcessor-Unterstützung nicht prüfen: " +
                hresultText(hr);
        return false;
    }

    UINT nv12Support = 0;
    hr = enumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_NV12, &nv12Support);
    if (FAILED(hr)) {
        error = "D3D11 konnte NV12-VideoProcessor-Unterstützung nicht prüfen: " +
                hresultText(hr);
        return false;
    }

    capabilities.videoProcessorAvailable = true;
    capabilities.bgra8InputSupported =
        (bgraSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;
    capabilities.nv12OutputSupported =
        (nv12Support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) != 0;
    error.clear();
    return true;
}

} // namespace streamforge

#endif // _WIN32
