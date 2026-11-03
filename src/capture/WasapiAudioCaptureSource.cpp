#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "WasapiAudioCaptureSource.h"

#ifdef _WIN32

#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <utility>
#include <vector>

namespace streamforge {
namespace {

using Microsoft::WRL::ComPtr;

enum class SampleKind { Float32, Pcm16, Pcm24, Pcm32 };

struct MixFormat {
    std::int32_t sampleRate{0};
    std::int32_t channels{0};
    std::int32_t bitsPerSample{0};
    std::int32_t blockAlign{0};
    SampleKind kind{SampleKind::Float32};
};

struct StartResult {
    bool ok{false};
    std::string error;
};

std::string hresultText(HRESULT result) {
    return "HRESULT 0x" + [] (unsigned long value) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string output(8, '0');
        for (int index = 7; index >= 0; --index) {
            output[static_cast<std::size_t>(index)] = digits[value & 0x0FU];
            value >>= 4U;
        }
        return output;
    }(static_cast<unsigned long>(result));
}

std::string captureErrorText(HRESULT result) {
    if (result == AUDCLNT_E_DEVICE_INVALIDATED) {
        return "Windows-Audio-Endpunkt wurde entfernt oder neu initialisiert ("
            + hresultText(result) + "). Capture muss nach erneuter Geräteauswahl neu gestartet werden.";
    }
    if (result == AUDCLNT_E_RESOURCES_INVALIDATED) {
        return "WASAPI-Audioressourcen wurden ungültig ("
            + hresultText(result) + "). Capture muss neu initialisiert werden.";
    }
    if (result == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        return "Der Windows-Audio-Dienst läuft nicht ("
            + hresultText(result) + ").";
    }
    return hresultText(result);
}

bool ensureEndpointActive(IMMDevice* device, std::string& error) {
    DWORD state = 0;
    const HRESULT result = device->GetState(&state);
    if (FAILED(result)) {
        error = "Status des WASAPI-Endpunkts konnte nicht gelesen werden: "
            + hresultText(result);
        return false;
    }
    if (state != DEVICE_STATE_ACTIVE) {
        error = "Der ausgewählte WASAPI-Endpunkt ist nicht aktiv. "
            "Bitte ein angeschlossenes und aktiviertes Gerät auswählen.";
        return false;
    }
    return true;
}

bool parseMixFormat(const WAVEFORMATEX* format, MixFormat& parsed,
                    std::string& error) {
    if (!format) {
        error = "WASAPI lieferte kein Mix-Format.";
        return false;
    }

    parsed.sampleRate = static_cast<std::int32_t>(format->nSamplesPerSec);
    parsed.channels = static_cast<std::int32_t>(format->nChannels);
    parsed.bitsPerSample = static_cast<std::int32_t>(format->wBitsPerSample);
    parsed.blockAlign = static_cast<std::int32_t>(format->nBlockAlign);

    if (parsed.sampleRate <= 0 ||
        (parsed.channels != 1 && parsed.channels != 2 &&
         parsed.channels != 6 && parsed.channels != 8)) {
        error = "WASAPI-Mix-Format verwendet eine nicht unterstützte Samplerate oder Kanalzahl.";
        return false;
    }

    bool isFloat = false;
    bool isPcm = false;
    std::int32_t validBits = parsed.bitsPerSample;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        isFloat = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        isPcm = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
        validBits = static_cast<std::int32_t>(
            extensible->Samples.wValidBitsPerSample);
    } else {
        isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        isPcm = format->wFormatTag == WAVE_FORMAT_PCM;
    }

    if (isFloat && parsed.bitsPerSample == 32 && validBits == 32) {
        parsed.kind = SampleKind::Float32;
    } else if (isPcm && validBits == parsed.bitsPerSample &&
               parsed.bitsPerSample == 16) {
        parsed.kind = SampleKind::Pcm16;
    } else if (isPcm && validBits == parsed.bitsPerSample &&
               parsed.bitsPerSample == 24) {
        parsed.kind = SampleKind::Pcm24;
    } else if (isPcm && validBits == parsed.bitsPerSample &&
               parsed.bitsPerSample == 32) {
        parsed.kind = SampleKind::Pcm32;
    } else {
        error = "WASAPI-Mix-Format muss Float32 oder PCM 16/24/32 Bit sein.";
        return false;
    }

    const std::int32_t bytesPerSample = parsed.bitsPerSample / 8;
    if (parsed.blockAlign != parsed.channels * bytesPerSample) {
        error = "WASAPI-Mix-Format hat eine ungültige Blockausrichtung.";
        return false;
    }
    return true;
}

float decodeSample(const std::uint8_t* sample, SampleKind kind) {
    float value = 0.0F;
    switch (kind) {
    case SampleKind::Float32:
        std::memcpy(&value, sample, sizeof(value));
        break;
    case SampleKind::Pcm16: {
        std::int16_t raw = 0;
        std::memcpy(&raw, sample, sizeof(raw));
        value = static_cast<float>(raw) / 32768.0F;
        break;
    }
    case SampleKind::Pcm24: {
        std::int32_t raw =
            static_cast<std::int32_t>(sample[0]) |
            (static_cast<std::int32_t>(sample[1]) << 8) |
            (static_cast<std::int32_t>(sample[2]) << 16);
        if ((raw & 0x00800000) != 0) raw |= static_cast<std::int32_t>(0xFF000000U);
        value = static_cast<float>(raw) / 8388608.0F;
        break;
    }
    case SampleKind::Pcm32: {
        std::int32_t raw = 0;
        std::memcpy(&raw, sample, sizeof(raw));
        value = static_cast<float>(
            static_cast<double>(raw) / 2147483648.0);
        break;
    }
    }
    if (!std::isfinite(value)) return 0.0F;
    return std::clamp(value, -1.0F, 1.0F);
}


AudioChannelLayout channelLayoutForChannels(std::int32_t channels) {
    switch (channels) {
    case 1: return AudioChannelLayout::Mono;
    case 2: return AudioChannelLayout::Stereo;
    case 6: return AudioChannelLayout::Surround51;
    case 8: return AudioChannelLayout::Surround71;
    default: return AudioChannelLayout::Unknown;
    }
}

bool convertPacket(const BYTE* data, UINT32 frames, DWORD flags,
                  UINT64 qpcPosition, const MixFormat& format,
                  AudioFrame& output, std::string& error) {
    if (frames == 0 ||
        frames > static_cast<UINT32>(std::numeric_limits<std::int32_t>::max())) {
        error = "WASAPI-Audiopaket hat eine ungültige Frameanzahl.";
        return false;
    }
    const auto frameCount = static_cast<std::size_t>(frames);
    const auto channels = static_cast<std::size_t>(format.channels);
    if (frameCount > std::numeric_limits<std::size_t>::max() / channels) {
        error = "WASAPI-Audiopaket ist zu groß.";
        return false;
    }

    output.sampleRate = format.sampleRate;
    output.channels = format.channels;
    output.frameCount = static_cast<std::int32_t>(frames);
    output.samples.resize(frameCount * channels);

    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (!silent && !data) {
        error = "WASAPI lieferte einen leeren Audiopuffer.";
        return false;
    }

    if (silent) {
        std::fill(output.samples.begin(), output.samples.end(), 0.0F);
    } else {
        const auto bytesPerSample =
            static_cast<std::size_t>(format.bitsPerSample / 8);
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            const auto* frameData =
                data + frame * static_cast<std::size_t>(format.blockAlign);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto* sample = frameData + channel * bytesPerSample;
                output.samples[frame * channels + channel] =
                    decodeSample(sample, format.kind);
            }
        }
    }

    if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 &&
        qpcPosition > 0 &&
        qpcPosition <= static_cast<UINT64>(
            std::numeric_limits<std::int64_t>::max() / 100)) {
        output.timestampNs = static_cast<std::int64_t>(qpcPosition * 100U);
    } else {
        output.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    return true;
}

} // namespace

struct WasapiAudioCaptureSource::Impl final {
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

    void run(const AudioCaptureConfig& config,
             AudioCaptureCallback callback,
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
            const std::string message = "COM konnte nicht initialisiert werden: " +
                                        hresultText(comResult);
            setError(message);
            publishStartup({false, message});
            return;
        }

        HANDLE audioEvent = nullptr;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioCaptureClient> captureClient;
        auto cleanup = [&] {
            captureClient.Reset();
            audioClient.Reset();
            device.Reset();
            enumerator.Reset();
            if (audioEvent) CloseHandle(audioEvent);
            if (comInitialized) CoUninitialize();
            running.store(false, std::memory_order_release);
        };

        try {
            HRESULT hr = CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator));
            if (FAILED(hr)) {
                const std::string message =
                    "MMDeviceEnumerator konnte nicht erstellt werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            if (config.deviceId.empty()) {
                hr = enumerator->GetDefaultAudioEndpoint(
                    config.loopback ? eRender : eCapture, eConsole, &device);
            } else {
                hr = enumerator->GetDevice(config.deviceId.c_str(), &device);
            }
            if (FAILED(hr)) {
                const std::string message =
                    "Audio-Endpunkt konnte nicht geöffnet werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }
            std::string endpointError;
            if (!ensureEndpointActive(device.Get(), endpointError)) {
                setError(endpointError);
                publishStartup({false, endpointError});
                cleanup();
                return;
            }

            hr = device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(audioClient.GetAddressOf()));
            if (FAILED(hr)) {
                const std::string message =
                    "IAudioClient konnte nicht aktiviert werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            WAVEFORMATEX* mixFormat = nullptr;
            hr = audioClient->GetMixFormat(&mixFormat);
            if (FAILED(hr) || !mixFormat) {
                if (mixFormat) CoTaskMemFree(mixFormat);
                const std::string message =
                    "WASAPI-Mix-Format konnte nicht gelesen werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            MixFormat parsedFormat;
            std::string formatError;
            const bool formatOk = parseMixFormat(mixFormat, parsedFormat, formatError);
            const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_NOPERSIST |
                (config.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0U);
            const REFERENCE_TIME duration =
                static_cast<REFERENCE_TIME>(config.bufferDurationMs) * 10'000;
            if (!formatOk) {
                CoTaskMemFree(mixFormat);
                setError(formatError);
                publishStartup({false, formatError});
                cleanup();
                return;
            }

            hr = audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED, streamFlags,
                duration, 0, mixFormat, nullptr);
            CoTaskMemFree(mixFormat);
            if (FAILED(hr)) {
                const std::string message =
                    "WASAPI-AudioClient konnte nicht initialisiert werden: " +
                    hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!audioEvent) {
                const std::string message = "WASAPI-Audioevent konnte nicht erstellt werden.";
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }
            hr = audioClient->SetEventHandle(audioEvent);
            if (FAILED(hr)) {
                const std::string message =
                    "WASAPI-Audioevent konnte nicht gesetzt werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
            if (FAILED(hr)) {
                const std::string message =
                    "IAudioCaptureClient konnte nicht abgerufen werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            hr = audioClient->Start();
            if (FAILED(hr)) {
                const std::string message =
                    "WASAPI-Capture konnte nicht gestartet werden: " + hresultText(hr);
                setError(message);
                publishStartup({false, message});
                cleanup();
                return;
            }

            running.store(true, std::memory_order_release);
            publishStartup({true, {}});

            HANDLE handles[2] = {stopEvent, audioEvent};
            bool keepRunning = true;
            while (keepRunning) {
                const DWORD waitResult = WaitForMultipleObjects(
                    2, handles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) break;
                if (waitResult != WAIT_OBJECT_0 + 1) {
                    setError("WASAPI-Capture-Worker erhielt ein ungültiges Wait-Ergebnis.");
                    break;
                }

                for (;;) {
                    UINT32 packetFrames = 0;
                    hr = captureClient->GetNextPacketSize(&packetFrames);
                    if (FAILED(hr)) {
                        setError("WASAPI-Paketgröße konnte nicht gelesen werden: " +
                                 captureErrorText(hr));
                        keepRunning = false;
                        break;
                    }
                    if (packetFrames == 0) break;

                    BYTE* captureData = nullptr;
                    UINT32 framesAvailable = 0;
                    DWORD bufferFlags = 0;
                    UINT64 devicePosition = 0;
                    UINT64 qpcPosition = 0;
                    hr = captureClient->GetBuffer(
                        &captureData, &framesAvailable, &bufferFlags,
                        &devicePosition, &qpcPosition);
                    (void)devicePosition;
                    if (FAILED(hr)) {
                        setError("WASAPI-Audiopuffer konnte nicht gelesen werden: " +
                                 captureErrorText(hr));
                        keepRunning = false;
                        break;
                    }

                    AudioFrame frame;
                    std::string conversionError;
                    const bool converted = convertPacket(
                        captureData, framesAvailable, bufferFlags, qpcPosition,
                        parsedFormat, frame, conversionError);
                    const HRESULT releaseResult =
                        captureClient->ReleaseBuffer(framesAvailable);
                    if (FAILED(releaseResult)) {
                        setError("WASAPI-Audiopuffer konnte nicht freigegeben werden: " +
                                 hresultText(releaseResult));
                        keepRunning = false;
                        break;
                    }
                    if (!converted) {
                        setError(conversionError);
                        keepRunning = false;
                        break;
                    }

                    std::string callbackError;
                    bool accepted = false;
                    try {
                        accepted = callback(std::move(frame), callbackError);
                    } catch (...) {
                        callbackError = "Ausnahme im WASAPI-Audio-Capture-Callback.";
                    }
                    if (!accepted) {
                        setError(callbackError.empty()
                            ? "Audio-Capture-Callback hat den Frame abgelehnt."
                            : callbackError);
                        keepRunning = false;
                        break;
                    }
                }
            }

            (void)audioClient->Stop();
        } catch (const std::exception& ex) {
            const std::string message =
                std::string("WASAPI-Capture-Ausnahme: ") + ex.what();
            setError(message);
            publishStartup({false, message});
        } catch (...) {
            const std::string message = "Unbekannte Ausnahme im WASAPI-Capture-Worker.";
            setError(message);
            publishStartup({false, message});
        }

        cleanup();
    }
};

WasapiAudioCaptureSource::WasapiAudioCaptureSource()
    : m_impl(std::make_unique<Impl>()) {}

WasapiAudioCaptureSource::~WasapiAudioCaptureSource() {
    std::string ignored;
    (void)stop(ignored);
}


bool WasapiAudioCaptureSource::queryFormat(
    const AudioCaptureConfig& config,
    AudioCaptureFormat& format,
    std::string& error) const {
    format = {};
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    const bool comAlreadyInitialized = comResult == RPC_E_CHANGED_MODE;
    if (FAILED(comResult) && !comAlreadyInitialized) {
        error = "COM konnte für WASAPI-Formatabfrage nicht initialisiert werden: " +
                hresultText(comResult);
        return false;
    }

    bool ok = false;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        WAVEFORMATEX* mixFormat = nullptr;

        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            if (config.deviceId.empty()) {
                hr = enumerator->GetDefaultAudioEndpoint(
                    config.loopback ? eRender : eCapture, eConsole, &device);
            } else {
                hr = enumerator->GetDevice(config.deviceId.c_str(), &device);
            }
        }
        if (SUCCEEDED(hr)) {
            std::string endpointError;
            if (!ensureEndpointActive(device.Get(), endpointError)) {
                error = endpointError;
                hr = E_FAIL;
            }
        }
        if (SUCCEEDED(hr)) {
            hr = device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(audioClient.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) hr = audioClient->GetMixFormat(&mixFormat);

        if (FAILED(hr) || !mixFormat) {
            if (error.empty()) {
                error = "WASAPI-Eingabeformat konnte nicht abgefragt werden: " +
                        hresultText(hr);
            }
        } else {
            MixFormat parsed;
            if (parseMixFormat(mixFormat, parsed, error)) {
                format.sampleRate = parsed.sampleRate;
                format.channels = parsed.channels;
                format.sampleFormat = AudioSampleFormat::Float32;
                format.channelLayout = channelLayoutForChannels(parsed.channels);
                ok = format.isValid();
                if (!ok) error = "WASAPI-Format ist nicht mit dem Audioencoder kompatibel.";
            }
        }

        if (mixFormat) CoTaskMemFree(mixFormat);
        audioClient.Reset();
        device.Reset();
        enumerator.Reset();
    }

    if (comInitialized) CoUninitialize();
    return ok;
}

bool WasapiAudioCaptureSource::start(
    const AudioCaptureConfig& config,
    AudioCaptureCallback callback,
    std::string& error) {
    if (m_impl->worker.joinable() || m_impl->running.load(std::memory_order_acquire)) {
        error = "WASAPI-Audio-Capture läuft bereits.";
        return false;
    }
    if (!callback || config.bufferDurationMs == 0) {
        error = "WASAPI-Capture benötigt Callback und positive Pufferdauer.";
        return false;
    }

    {
        std::lock_guard lock(m_impl->errorMutex);
        m_impl->workerError.clear();
    }
    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->stopEvent) {
        error = "WASAPI-Stop-Event konnte nicht erstellt werden.";
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
        error = std::string("WASAPI-Worker konnte nicht gestartet werden: ") + ex.what();
        return false;
    }

    StartResult result;
    try {
        result = future.get();
    } catch (const std::exception& ex) {
        error = std::string("WASAPI-Startsignal fehlgeschlagen: ") + ex.what();
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

bool WasapiAudioCaptureSource::stop(std::string& error) {
    if (m_impl->stopEvent) SetEvent(m_impl->stopEvent);
    if (m_impl->worker.joinable()) m_impl->worker.join();
    if (m_impl->stopEvent) {
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
    }
    error = m_impl->getError();
    return error.empty();
}

bool WasapiAudioCaptureSource::isRunning() const noexcept {
    return m_impl->running.load(std::memory_order_acquire);
}

} // namespace streamforge

#else

namespace streamforge {

struct WasapiAudioCaptureSource::Impl {};

WasapiAudioCaptureSource::WasapiAudioCaptureSource()
    : m_impl(std::make_unique<Impl>()) {}
WasapiAudioCaptureSource::~WasapiAudioCaptureSource() = default;

bool WasapiAudioCaptureSource::queryFormat(
    const AudioCaptureConfig&, AudioCaptureFormat&, std::string& error) const {
    error = "WASAPI ist nur unter Windows verfügbar.";
    return false;
}
bool WasapiAudioCaptureSource::start(
    const AudioCaptureConfig&, AudioCaptureCallback, std::string& error) {
    error = "WASAPI ist nur unter Windows verfügbar.";
    return false;
}
bool WasapiAudioCaptureSource::stop(std::string& error) {
    error.clear();
    return true;
}
bool WasapiAudioCaptureSource::isRunning() const noexcept { return false; }

} // namespace streamforge

#endif
