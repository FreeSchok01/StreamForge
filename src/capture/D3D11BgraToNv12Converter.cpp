#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "D3D11BgraToNv12Converter.h"
#include "D3D11VideoProcessorCapabilities.h"

#ifdef _WIN32

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>
#include <utility>

namespace streamforge {
namespace {

using Microsoft::WRL::ComPtr;

struct Nv12TexturePool final {
    std::mutex mutex;
    std::vector<ComPtr<ID3D11Texture2D>> available;

    bool acquire(ComPtr<ID3D11Texture2D>& texture) {
        std::lock_guard lock(mutex);
        if (available.empty()) return false;
        texture = std::move(available.back());
        available.pop_back();
        return true;
    }

    void recycle(ComPtr<ID3D11Texture2D> texture) noexcept {
        if (!texture) return;
        try {
            std::lock_guard lock(mutex);
            available.push_back(std::move(texture));
        } catch (...) {
            // Releasing the texture is a safe fallback if the free-list cannot grow.
        }
    }
};

struct Nv12FrameLease final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::shared_ptr<std::recursive_mutex> contextMutex;
    std::shared_ptr<Nv12TexturePool> pool;

    ~Nv12FrameLease() {
        if (pool && texture) pool->recycle(std::move(texture));
    }
};

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

bool isNativeBgraFrame(const VideoFrameInput& frame) {
    return frame.isValid() &&
           frame.nativeResourceType == NativeVideoResourceType::D3D11Texture2D &&
           frame.pixelFormat == PixelFormat::BGRA8 &&
           frame.nativeResource && frame.nativeDevice && frame.nativeContext &&
           frame.nativeContextMutex && frame.nativeResourceOwner;
}

} // namespace

struct D3D11BgraToNv12Converter::Impl final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    std::shared_ptr<std::recursive_mutex> contextMutex;
    std::shared_ptr<Nv12TexturePool> texturePool;
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool initialized{false};

    void reset() noexcept {
        processor.Reset();
        enumerator.Reset();
        videoContext.Reset();
        videoDevice.Reset();
        context.Reset();
        device.Reset();
        contextMutex.reset();
        texturePool.reset();
        width = 0;
        height = 0;
        initialized = false;
    }
};

D3D11BgraToNv12Converter::D3D11BgraToNv12Converter()
    : m_impl(std::make_unique<Impl>()) {}

D3D11BgraToNv12Converter::~D3D11BgraToNv12Converter() {
    shutdown();
}

bool D3D11BgraToNv12Converter::initialize(
    const VideoFrameInput& firstFrame,
    std::uint32_t fpsNumerator,
    std::uint32_t fpsDenominator,
    std::string& error) {
    if (m_impl->initialized) {
        error = "D3D11-BGRA-NV12-Konverter ist bereits initialisiert.";
        return false;
    }
    if (!isNativeBgraFrame(firstFrame) ||
        (firstFrame.width % 2) != 0 || (firstFrame.height % 2) != 0 ||
        fpsNumerator == 0 || fpsDenominator == 0) {
        error = "D3D11-BGRA-NV12-Konverter benötigt einen gültigen nativen BGRA-Frame mit geraden Abmessungen und gültiger Framerate.";
        return false;
    }

    auto* device = static_cast<ID3D11Device*>(firstFrame.nativeDevice);
    auto* context = static_cast<ID3D11DeviceContext*>(firstFrame.nativeContext);
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "D3D11-BGRA-NV12-Konverter benötigt den Immediate Context des Capture-Geräts.";
        return false;
    }

    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != device) {
        error = "D3D11-Device und Immediate Context des Eingabeframes gehören nicht zusammen.";
        return false;
    }

    D3D11VideoProcessorCapabilities capabilities;
    if (!queryD3D11BgraToNv12Capabilities(
            device, static_cast<std::uint32_t>(firstFrame.width),
            static_cast<std::uint32_t>(firstFrame.height),
            fpsNumerator, fpsDenominator, capabilities, error)) {
        return false;
    }
    if (!capabilities.supportsBgraToNv12()) {
        error = "Das D3D11-Gerät unterstützt BGRA8-Eingabe und NV12-Ausgabe im Video-Processor nicht gleichzeitig.";
        return false;
    }

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_impl->videoDevice));
    if (FAILED(hr)) {
        error = "ID3D11VideoDevice ist nicht verfügbar: " + hresultText(hr);
        m_impl->reset();
        return false;
    }
    hr = context->QueryInterface(IID_PPV_ARGS(&m_impl->videoContext));
    if (FAILED(hr)) {
        error = "ID3D11VideoContext ist nicht verfügbar: " + hresultText(hr);
        m_impl->reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
    description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    description.InputFrameRate = {fpsNumerator, fpsDenominator};
    description.InputWidth = static_cast<UINT>(firstFrame.width);
    description.InputHeight = static_cast<UINT>(firstFrame.height);
    description.OutputFrameRate = description.InputFrameRate;
    description.OutputWidth = description.InputWidth;
    description.OutputHeight = description.InputHeight;
    description.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = m_impl->videoDevice->CreateVideoProcessorEnumerator(
        &description, &m_impl->enumerator);
    if (FAILED(hr)) {
        error = "D3D11-VideoProcessor-Enumerator konnte nicht erstellt werden: " +
                hresultText(hr);
        m_impl->reset();
        return false;
    }
    hr = m_impl->videoDevice->CreateVideoProcessor(
        m_impl->enumerator.Get(), 0, &m_impl->processor);
    if (FAILED(hr)) {
        error = "D3D11-VideoProcessor konnte nicht erstellt werden: " +
                hresultText(hr);
        m_impl->reset();
        return false;
    }

    m_impl->device = device;
    m_impl->context = context;
    m_impl->contextMutex = firstFrame.nativeContextMutex;
    try {
        m_impl->texturePool = std::make_shared<Nv12TexturePool>();
    } catch (...) {
        error = "NV12-Texturpool konnte nicht erstellt werden.";
        m_impl->reset();
        return false;
    }
    m_impl->width = static_cast<std::uint32_t>(firstFrame.width);
    m_impl->height = static_cast<std::uint32_t>(firstFrame.height);
    m_impl->initialized = true;
    error.clear();
    return true;
}

bool D3D11BgraToNv12Converter::convert(
    const VideoFrameInput& input,
    OwnedVideoFrame& output,
    std::string& error) {
    if (!m_impl->initialized) {
        error = "D3D11-BGRA-NV12-Konverter ist nicht initialisiert.";
        return false;
    }
    if (!isNativeBgraFrame(input) ||
        input.width != static_cast<std::int32_t>(m_impl->width) ||
        input.height != static_cast<std::int32_t>(m_impl->height) ||
        input.nativeDevice != m_impl->device.Get() ||
        input.nativeContext != m_impl->context.Get() ||
        input.nativeContextMutex != m_impl->contextMutex) {
        error = "D3D11-Eingabeframe passt nicht zum initialisierten BGRA-NV12-Konverter oder teilt nicht dessen synchronisierten Context.";
        return false;
    }

    auto* inputTexture = static_cast<ID3D11Texture2D*>(input.nativeResource);
    D3D11_TEXTURE2D_DESC inputDesc{};
    inputTexture->GetDesc(&inputDesc);
    if (inputDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        inputDesc.Width != m_impl->width ||
        inputDesc.Height != m_impl->height ||
        inputDesc.SampleDesc.Count != 1) {
        error = "D3D11-Eingabetextur muss eine nicht-multisampled BGRA8-UNORM-Textur in der konfigurierten Auflösung sein.";
        return false;
    }
    ComPtr<ID3D11Device> inputDevice;
    inputTexture->GetDevice(&inputDevice);
    if (inputDevice.Get() != m_impl->device.Get()) {
        error = "D3D11-Eingabetextur gehört nicht zum initialisierten Capture-Gerät.";
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDesc{};
    outputDesc.Width = m_impl->width;
    outputDesc.Height = m_impl->height;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_NV12;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
    outputDesc.CPUAccessFlags = 0;
    outputDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> outputTexture;
    if (!m_impl->texturePool->acquire(outputTexture)) {
        const HRESULT createResult = m_impl->device->CreateTexture2D(
            &outputDesc, nullptr, &outputTexture);
        if (FAILED(createResult)) {
            error = "D3D11-NV12-Ausgabetextur konnte nicht erstellt werden: " +
                    hresultText(createResult);
            return false;
        }
    }
    HRESULT hr = S_OK;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    // Zero asks D3D11 to use the texture's DXGI format. This is the
    // documented path for ordinary DXGI_FORMAT_* resources (FourCC is for
    // explicitly tagged surface formats).
    inputViewDesc.FourCC = 0;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;
    inputViewDesc.Texture2D.ArraySlice = 0;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    hr = m_impl->videoDevice->CreateVideoProcessorInputView(
        inputTexture, m_impl->enumerator.Get(), &inputViewDesc, &inputView);
    if (FAILED(hr)) {
        error = "D3D11-VideoProcessor-Eingabeansicht konnte nicht erstellt werden: " +
                hresultText(hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    hr = m_impl->videoDevice->CreateVideoProcessorOutputView(
        outputTexture.Get(), m_impl->enumerator.Get(),
        &outputViewDesc, &outputView);
    if (FAILED(hr)) {
        error = "D3D11-VideoProcessor-Ausgabeansicht konnte nicht erstellt werden: " +
                hresultText(hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    {
        std::lock_guard contextLock(*m_impl->contextMutex);
        m_impl->videoContext->VideoProcessorSetStreamFrameFormat(
            m_impl->processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_impl->videoContext->VideoProcessorSetStreamAutoProcessingMode(
            m_impl->processor.Get(), 0, FALSE);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
        inputColorSpace.RGB_Range = 0; // Full-range RGB input.
        inputColorSpace.YCbCr_Matrix = 1; // BT.709 matrix.
        inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
        outputColorSpace.YCbCr_Matrix = 1; // BT.709 matrix.
        outputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_impl->videoContext->VideoProcessorSetStreamColorSpace(
            m_impl->processor.Get(), 0, &inputColorSpace);
        m_impl->videoContext->VideoProcessorSetOutputColorSpace(
            m_impl->processor.Get(), &outputColorSpace);

        hr = m_impl->videoContext->VideoProcessorBlt(
            m_impl->processor.Get(), outputView.Get(), 0, 1, &stream);
    }
    if (FAILED(hr)) {
        error = "D3D11-VideoProcessorBlt für BGRA-zu-NV12-Konvertierung fehlgeschlagen: " +
                hresultText(hr);
        return false;
    }

    try {
        auto lease = std::make_shared<Nv12FrameLease>();
        lease->texture = std::move(outputTexture);
        lease->device = m_impl->device;
        lease->context = m_impl->context;
        lease->contextMutex = m_impl->contextMutex;
        lease->pool = m_impl->texturePool;

        output = {};
        output.width = static_cast<std::int32_t>(m_impl->width);
        output.height = static_cast<std::int32_t>(m_impl->height);
        output.pixelFormat = PixelFormat::NV12;
        output.pts = input.pts;
        output.timestampNs = input.timestampNs;
        output.timebase = input.timebase;
        output.nativeResourceType = NativeVideoResourceType::D3D11Texture2D;
        output.nativeResource = lease->texture.Get();
        output.nativeDevice = lease->device.Get();
        output.nativeContext = lease->context.Get();
        output.nativeContextMutex = lease->contextMutex;
        output.nativeResourceOwner = std::move(lease);
    } catch (...) {
        output = {};
        error = "Lebensdauerhalter für die D3D11-NV12-Ausgabetextur konnte nicht erstellt werden.";
        return false;
    }

    if (!output.isValid()) {
        output = {};
        error = "D3D11-BGRA-NV12-Konvertierung erzeugte einen ungültigen nativen Frame.";
        return false;
    }
    error.clear();
    return true;
}

void D3D11BgraToNv12Converter::shutdown() noexcept {
    if (m_impl) m_impl->reset();
}

} // namespace streamforge

#endif // _WIN32
