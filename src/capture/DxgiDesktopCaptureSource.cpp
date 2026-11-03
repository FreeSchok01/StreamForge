#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "DxgiDesktopCaptureSource.h"

#ifdef _WIN32

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace streamforge {
namespace {

using Microsoft::WRL::ComPtr;

struct D3D11FrameLease final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::shared_ptr<std::mutex> contextMutex;
};

struct StartResult {
    bool ok{false};
    std::string error;
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

std::int64_t qpcToNanoseconds(
    std::int64_t qpc, std::int64_t frequency) {
    if (qpc <= 0 || frequency <= 0) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    const long double ns =
        (static_cast<long double>(qpc) * 1'000'000'000.0L) /
        static_cast<long double>(frequency);
    if (ns > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    return static_cast<std::int64_t>(ns);
}

bool copyDesktopFrame(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    std::int64_t timestampNs,
    bool includeCpuReadback,
    bool includeNativeResource,
    const std::shared_ptr<std::mutex>& contextMutex,
    ComPtr<ID3D11Texture2D>& staging,
    OwnedVideoFrame& output,
    std::string& error) {

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        error = "DXGI Desktop Duplication lieferte kein BGRA8-Format.";
        return false;
    }
    if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
        sourceDesc.Width > static_cast<UINT>(INT32_MAX / 4) ||
        sourceDesc.Height > static_cast<UINT>(INT32_MAX)) {
        error = "DXGI-Desktopauflösung ist ungültig oder zu groß.";
        return false;
    }

    output = {};
    output.width = static_cast<std::int32_t>(sourceDesc.Width);
    output.height = static_cast<std::int32_t>(sourceDesc.Height);
    output.pixelFormat = PixelFormat::BGRA8;
    output.timestampNs = timestampNs;
    output.timebase = {1, 1'000'000'000};

    if (includeNativeResource) {
        // Make an independently owned GPU texture before ReleaseFrame().
        // This is a GPU copy, not zero-copy capture.
        D3D11_TEXTURE2D_DESC nativeDesc = sourceDesc;
        nativeDesc.Usage = D3D11_USAGE_DEFAULT;
        nativeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        nativeDesc.CPUAccessFlags = 0;
        nativeDesc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> nativeTexture;
        const HRESULT nativeResult =
            device->CreateTexture2D(&nativeDesc, nullptr, &nativeTexture);
        if (FAILED(nativeResult)) {
            error = "D3D11-Frame-Textur für native Übergabe konnte nicht erstellt werden: " +
                    hresultText(nativeResult);
            return false;
        }
        {
            std::lock_guard contextLock(*contextMutex);
            context->CopyResource(nativeTexture.Get(), source);
        }

        try {
            auto lease = std::make_shared<D3D11FrameLease>();
            lease->texture = nativeTexture;
            lease->device = device;
            lease->context = context;
            lease->contextMutex = contextMutex;
            output.nativeResourceOwner = std::move(lease);
        } catch (...) {
            output = {};
            error = "Lebensdauerhalter für native D3D11-Frame-Textur und Gerät konnte nicht erstellt werden.";
            return false;
        }
        const auto lease =
            std::static_pointer_cast<D3D11FrameLease>(output.nativeResourceOwner);
        output.nativeResourceType = NativeVideoResourceType::D3D11Texture2D;
        output.nativeResource = lease->texture.Get();
        output.nativeDevice = lease->device.Get();
        output.nativeContext = lease->context.Get();
        output.nativeContextMutex = lease->contextMutex;
    }

    if (includeCpuReadback) {
        D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;

        bool recreate = !staging;
        if (staging) {
            D3D11_TEXTURE2D_DESC current{};
            staging->GetDesc(&current);
            recreate = current.Width != stagingDesc.Width ||
                       current.Height != stagingDesc.Height ||
                       current.Format != stagingDesc.Format;
        }
        if (recreate) {
            staging.Reset();
            const HRESULT createResult =
                device->CreateTexture2D(&stagingDesc, nullptr, &staging);
            if (FAILED(createResult)) {
                error = "DXGI-Staging-Textur konnte nicht erstellt werden: " +
                        hresultText(createResult);
                return false;
            }
        }

        std::lock_guard contextLock(*contextMutex);
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult =
            context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult)) {
            error = "DXGI-Staging-Textur konnte nicht gelesen werden: " +
                    hresultText(mapResult);
            return false;
        }

        output.linesize[0] = output.width * 4;
        const auto stride = static_cast<std::size_t>(output.linesize[0]);
        const auto rows = static_cast<std::size_t>(output.height);
        if (rows > std::numeric_limits<std::size_t>::max() / stride) {
            context->Unmap(staging.Get(), 0);
            error = "DXGI-Framegröße überschreitet den Adressraum.";
            return false;
        }
        const auto* sourceBytes =
            static_cast<const std::uint8_t*>(mapped.pData);
        if (!sourceBytes || mapped.RowPitch < static_cast<UINT>(output.linesize[0])) {
            context->Unmap(staging.Get(), 0);
            error = "DXGI-Mapped-Textur hat einen ungültigen Zeilenabstand.";
            return false;
        }
        try {
            output.planes[0].resize(stride * rows);
        } catch (...) {
            context->Unmap(staging.Get(), 0);
            error = "Speicher für DXGI-CPU-Frame konnte nicht reserviert werden.";
            return false;
        }
        for (std::size_t row = 0; row < rows; ++row) {
            std::memcpy(
                output.planes[0].data() + row * stride,
                sourceBytes + row * static_cast<std::size_t>(mapped.RowPitch),
                stride);
        }
        context->Unmap(staging.Get(), 0);
    }

    if (!output.isValid()) {
        error = "DXGI lieferte einen ungültigen CPU-/GPU-Frame.";
        return false;
    }
    return true;
}

} // namespace

struct DxgiDesktopCaptureSource::Impl final {
    std::thread worker;
    HANDLE stopEvent{nullptr};
    std::atomic<bool> running{false};
    mutable std::mutex errorMutex;
    std::string workerError;

    void setError(std::string error) {
        std::lock_guard lock(errorMutex);
        workerError = std::move(error);
    }

    std::string getError() const {
        std::lock_guard lock(errorMutex);
        return workerError;
    }

    void run(const VideoCaptureConfig& config,
             VideoCaptureCallback callback,
             std::promise<StartResult> startup) {
        bool startupPublished = false;
        auto publishStartup = [&](StartResult result) {
            if (!startupPublished) {
                startupPublished = true;
                startup.set_value(std::move(result));
            }
        };

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitialized = SUCCEEDED(comResult);
        if (FAILED(comResult)) {
            const std::string message =
                "COM konnte für DXGI-Capture nicht initialisiert werden: " +
                hresultText(comResult);
            setError(message);
            publishStartup({false, message});
            return;
        }

        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput1> output1;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGIOutputDuplication> duplication;
        ComPtr<ID3D11Texture2D> staging;
        std::shared_ptr<std::mutex> contextMutex;

        auto cleanup = [&] {
            staging.Reset();
            duplication.Reset();
            context.Reset();
            device.Reset();
            output1.Reset();
            output.Reset();
            adapter.Reset();
            factory.Reset();
            if (comInitialized) CoUninitialize();
            running.store(false, std::memory_order_release);
        };

        try {
            HRESULT hr = CreateDXGIFactory1(
                __uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(factory.GetAddressOf()));
            if (FAILED(hr)) {
                const std::string message =
                    "DXGI-Factory konnte nicht erstellt werden: " + hresultText(hr);
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            hr = factory->EnumAdapters1(config.adapterIndex, &adapter);
            if (FAILED(hr)) {
                const std::string message =
                    "DXGI-Adapter konnte nicht gefunden werden: " + hresultText(hr);
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            hr = adapter->EnumOutputs(config.outputIndex, &output);
            if (FAILED(hr)) {
                const std::string message =
                    "DXGI-Ausgabe konnte nicht gefunden werden: " + hresultText(hr);
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            hr = output.As(&output1);
            if (FAILED(hr)) {
                const std::string message =
                    "IDXGIOutput1 ist nicht verfügbar: " + hresultText(hr);
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            constexpr D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
            };
            D3D_FEATURE_LEVEL selectedLevel{};
            hr = D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION, &device, &selectedLevel, &context);
            if (hr == E_INVALIDARG) {
                hr = D3D11CreateDevice(
                    adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    &levels[1], static_cast<UINT>(std::size(levels) - 1U),
                    D3D11_SDK_VERSION, &device, &selectedLevel, &context);
            }
            if (FAILED(hr)) {
                const std::string message =
                    "D3D11-Gerät für Desktop-Capture konnte nicht erstellt werden: " +
                    hresultText(hr);
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            contextMutex = std::make_shared<std::mutex>();

            hr = output1->DuplicateOutput(device.Get(), &duplication);
            if (FAILED(hr)) {
                const std::string message =
                    "DXGI Desktop Duplication konnte nicht gestartet werden: " +
                    hresultText(hr);
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            LARGE_INTEGER qpcFrequency{};
            if (!QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart <= 0) {
                const std::string message = "QueryPerformanceFrequency ist fehlgeschlagen.";
                setError(message); publishStartup({false, message}); cleanup(); return;
            }

            running.store(true, std::memory_order_release);
            publishStartup({true, {}});

            bool hasFrameTimestamp = false;
            std::int64_t lastFrameTimestampNs = 0;
            while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
                DXGI_OUTDUPL_FRAME_INFO frameInfo{};
                ComPtr<IDXGIResource> resource;
                hr = duplication->AcquireNextFrame(
                    config.acquireTimeoutMs, &frameInfo, &resource);
                if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
                if (hr == DXGI_ERROR_ACCESS_LOST) {
                    // Desktop topology changes, lock/unlock, and display power
                    // transitions invalidate the duplication interface. Recreate
                    // it on the existing adapter/output before failing the session.
                    duplication.Reset();
                    constexpr unsigned kMaxRecoveryAttempts = 20;
                    bool recovered = false;
                    for (unsigned attempt = 0;
                         attempt < kMaxRecoveryAttempts &&
                         WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0;
                         ++attempt) {
                        const DWORD waitResult = WaitForSingleObject(stopEvent, 250);
                        if (waitResult == WAIT_OBJECT_0) break;
                        if (waitResult != WAIT_TIMEOUT) {
                            setError("DXGI-Wiederherstellung konnte den Stop-Event nicht abfragen.");
                            break;
                        }
                        hr = output1->DuplicateOutput(device.Get(), &duplication);
                        if (SUCCEEDED(hr)) {
                            recovered = true;
                            hasFrameTimestamp = false;
                            break;
                        }
                        if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE &&
                            hr != DXGI_ERROR_ACCESS_LOST &&
                            hr != DXGI_ERROR_INVALID_CALL) {
                            setError("DXGI Desktop Duplication konnte nicht wiederhergestellt werden: " +
                                     hresultText(hr));
                            break;
                        }
                    }
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
                    if (!recovered) {
                        if (getError().empty()) {
                            setError("DXGI Desktop Duplication blieb nach 20 Wiederherstellungsversuchen nicht verfügbar; "
                                     "Adapter-/Ausgabeneuinitialisierung erforderlich.");
                        }
                        break;
                    }
                    continue;
                }
                if (FAILED(hr)) {
                    setError("DXGI AcquireNextFrame fehlgeschlagen: " + hresultText(hr));
                    break;
                }

                struct FrameRelease final {
                    IDXGIOutputDuplication* duplication;
                    ~FrameRelease() { if (duplication) (void)duplication->ReleaseFrame(); }
                } release{duplication.Get()};

                ComPtr<ID3D11Texture2D> sourceTexture;
                hr = resource.As(&sourceTexture);
                if (FAILED(hr)) {
                    setError("DXGI-Frame ist keine D3D11-Textur: " + hresultText(hr));
                    break;
                }

                std::int64_t timestampNs = qpcToNanoseconds(
                    frameInfo.LastPresentTime.QuadPart, qpcFrequency.QuadPart);
                const std::int64_t acquisitionNowNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                if (!hasFrameTimestamp && timestampNs < acquisitionNowNs) {
                    // The initial duplication frame may describe an old desktop
                    // present. Anchor the first captured image at acquisition time.
                    timestampNs = acquisitionNowNs;
                } else if (hasFrameTimestamp && timestampNs <= lastFrameTimestampNs) {
                    timestampNs = lastFrameTimestampNs + 1;
                }
                lastFrameTimestampNs = timestampNs;
                hasFrameTimestamp = true;
                OwnedVideoFrame frame;
                std::string copyError;
                if (!copyDesktopFrame(
                        device.Get(), context.Get(), sourceTexture.Get(),
                        timestampNs, config.includeCpuReadback, config.includeNativeResource,
                        contextMutex, staging, frame, copyError)) {
                    setError(copyError.empty()
                        ? "DXGI-Frame konnte nicht kopiert werden." : copyError);
                    break;
                }

                // Release the duplication frame before handing the independently owned
                // CPU/GPU frame and its lifetime lease to downstream workers.
                const HRESULT releaseResult = duplication->ReleaseFrame();
                release.duplication = nullptr;
                if (FAILED(releaseResult)) {
                    setError("DXGI-Frame konnte nicht freigegeben werden: " +
                             hresultText(releaseResult));
                    break;
                }

                std::string callbackError;
                bool keepRunning = false;
                try {
                    keepRunning = callback(std::move(frame), callbackError);
                } catch (...) {
                    callbackError = "Ausnahme im DXGI-Video-Capture-Callback.";
                }
                if (!keepRunning) {
                    setError(callbackError.empty()
                        ? "Video-Capture-Callback hat den Frame abgelehnt."
                        : callbackError);
                    break;
                }
            }
        } catch (const std::exception& ex) {
            const std::string message =
                std::string("DXGI-Capture-Ausnahme: ") + ex.what();
            setError(message);
            publishStartup({false, message});
        } catch (...) {
            const std::string message = "Unbekannte Ausnahme im DXGI-Capture-Worker.";
            setError(message);
            publishStartup({false, message});
        }

        cleanup();
    }
};

DxgiDesktopCaptureSource::DxgiDesktopCaptureSource()
    : m_impl(std::make_unique<Impl>()) {}

DxgiDesktopCaptureSource::~DxgiDesktopCaptureSource() {
    std::string ignored;
    (void)stop(ignored);
}

bool DxgiDesktopCaptureSource::start(
    const VideoCaptureConfig& config,
    VideoCaptureCallback callback,
    std::string& error) {
    if (m_impl->worker.joinable() || m_impl->running.load(std::memory_order_acquire)) {
        error = "DXGI-Desktop-Capture läuft bereits.";
        return false;
    }
    if (!callback || config.acquireTimeoutMs == 0 ||
        config.acquireTimeoutMs > 1000 ||
        (!config.includeCpuReadback && !config.includeNativeResource)) {
        error = "DXGI-Capture benötigt Callback, Timeout zwischen 1 und 1000 ms und mindestens einen CPU-/GPU-Ausgabepfad.";
        return false;
    }

    {
        std::lock_guard lock(m_impl->errorMutex);
        m_impl->workerError.clear();
    }
    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->stopEvent) {
        error = "DXGI-Stop-Event konnte nicht erstellt werden.";
        return false;
    }

    std::promise<StartResult> startup;
    auto future = startup.get_future();
    try {
        m_impl->worker = std::thread(
            [this, config, callback = std::move(callback),
             startup = std::move(startup)]() mutable {
                m_impl->run(config, std::move(callback), std::move(startup));
            });
    } catch (const std::exception& ex) {
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
        error = std::string("DXGI-Worker konnte nicht gestartet werden: ") + ex.what();
        return false;
    }

    StartResult result;
    try {
        result = future.get();
    } catch (const std::exception& ex) {
        error = std::string("DXGI-Startsignal fehlgeschlagen: ") + ex.what();
        SetEvent(m_impl->stopEvent);
        if (m_impl->worker.joinable()) m_impl->worker.join();
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
        return false;
    }

    if (!result.ok) {
        error = result.error;
        if (m_impl->worker.joinable()) m_impl->worker.join();
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
        return false;
    }
    return true;
}

bool DxgiDesktopCaptureSource::stop(std::string& error) {
    if (m_impl->stopEvent) SetEvent(m_impl->stopEvent);
    if (m_impl->worker.joinable()) m_impl->worker.join();
    if (m_impl->stopEvent) {
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
    }
    error = m_impl->getError();
    return error.empty();
}

bool DxgiDesktopCaptureSource::isRunning() const noexcept {
    return m_impl->running.load(std::memory_order_acquire);
}

std::string DxgiDesktopCaptureSource::lastError() const {
    return m_impl->getError();
}

} // namespace streamforge

#endif
