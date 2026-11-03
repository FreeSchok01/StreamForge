#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "WasapiDeviceEnumerator.h"

#ifdef _WIN32

#include <Windows.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <vector>

namespace streamforge {
namespace {

using Microsoft::WRL::ComPtr;

// PKEY_Device_FriendlyName from the Windows property-key catalog
// (fmtid {A45C254E-DF1C-4EFD-8020-67D146A850E0}, PID 14).
constexpr PROPERTYKEY kDeviceFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

std::string hrText(HRESULT hr) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result(8, '0');
    unsigned long value = static_cast<unsigned long>(hr);
    for (int index = 7; index >= 0; --index) {
        result[static_cast<std::size_t>(index)] = digits[value & 0x0FU];
        value >>= 4U;
    }
    return "HRESULT 0x" + result;
}

EDataFlow toDataFlow(AudioDeviceFlow flow) {
    return flow == AudioDeviceFlow::Render ? eRender : eCapture;
}

struct ComApartment final {
    bool shouldUninitialize{false};
    ~ComApartment() { if (shouldUninitialize) CoUninitialize(); }
};

bool initializeCom(bool& shouldUninitialize, std::string& error) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    shouldUninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        error = "COM für WASAPI-Geräteabfrage fehlgeschlagen: " + hrText(hr);
        return false;
    }
    return true;
}

bool readDeviceInfo(IMMDevice* device, AudioDeviceFlow flow,
                    const std::wstring& defaultId,
                    AudioDeviceInfo& result, std::string& error) {
    LPWSTR rawId = nullptr;
    HRESULT hr = device->GetId(&rawId);
    if (FAILED(hr) || !rawId) {
        error = "WASAPI-Geräte-ID konnte nicht gelesen werden: " + hrText(hr);
        return false;
    }
    result.id = rawId;
    CoTaskMemFree(rawId);

    result.flow = flow;
    result.isDefault = result.id == defaultId;

    ComPtr<IPropertyStore> properties;
    hr = device->OpenPropertyStore(STGM_READ, &properties);
    if (SUCCEEDED(hr)) {
        PROPVARIANT value;
        PropVariantInit(&value);
        hr = properties->GetValue(kDeviceFriendlyNameKey, &value);
        if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) {
            result.name = value.pwszVal;
        }
        PropVariantClear(&value);
    }

    if (result.name.empty()) result.name = result.id;
    return true;
}

} // namespace

bool WasapiDeviceEnumerator::enumerate(
    AudioDeviceFlow flow, std::vector<AudioDeviceInfo>& devices,
    std::string& error) const {
    devices.clear();
    bool shouldUninitialize = false;
    if (!initializeCom(shouldUninitialize, error)) return false;
    ComApartment apartment{shouldUninitialize};

    bool ok = false;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            error = "MMDeviceEnumerator konnte nicht erstellt werden: " + hrText(hr);
        } else {
            std::wstring defaultId;
            ComPtr<IMMDevice> defaultDevice;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                    toDataFlow(flow), eConsole, &defaultDevice))) {
                LPWSTR rawDefaultId = nullptr;
                if (SUCCEEDED(defaultDevice->GetId(&rawDefaultId)) && rawDefaultId) {
                    defaultId = rawDefaultId;
                    CoTaskMemFree(rawDefaultId);
                }
            }

            ComPtr<IMMDeviceCollection> collection;
            hr = enumerator->EnumAudioEndpoints(
                toDataFlow(flow), DEVICE_STATE_ACTIVE, &collection);
            if (FAILED(hr)) {
                error = "WASAPI-Audioendpunkte konnten nicht aufgelistet werden: " +
                        hrText(hr);
            } else {
                UINT count = 0;
                hr = collection->GetCount(&count);
                if (FAILED(hr)) {
                    error = "WASAPI-Geräteanzahl konnte nicht gelesen werden: " +
                            hrText(hr);
                } else {
                    ok = true;
                    for (UINT index = 0; index < count; ++index) {
                        ComPtr<IMMDevice> endpoint;
                        hr = collection->Item(index, &endpoint);
                        if (FAILED(hr)) {
                            error = "WASAPI-Endpunkt konnte nicht gelesen werden: " +
                                    hrText(hr);
                            ok = false;
                            break;
                        }

                        AudioDeviceInfo info;
                        if (!readDeviceInfo(
                                endpoint.Get(), flow, defaultId, info, error)) {
                            ok = false;
                            break;
                        }
                        devices.push_back(std::move(info));
                    }
                }
            }
        }
        collection.Reset();
        defaultDevice.Reset();
        enumerator.Reset();
    }

    return ok;
}

bool WasapiDeviceEnumerator::getDefault(
    AudioDeviceFlow flow, AudioDeviceInfo& device,
    std::string& error) const {
    std::vector<AudioDeviceInfo> devices;
    if (!enumerate(flow, devices, error)) return false;
    for (auto& candidate : devices) {
        if (candidate.isDefault) {
            device = std::move(candidate);
            return true;
        }
    }
    error = "Für diesen WASAPI-Gerätefluss ist kein Standardendpunkt verfügbar.";
    return false;
}

} // namespace streamforge

#else

namespace streamforge {

bool WasapiDeviceEnumerator::enumerate(
    AudioDeviceFlow, std::vector<AudioDeviceInfo>& devices,
    std::string& error) const {
    devices.clear();
    error = "WASAPI-Geräte sind nur unter Windows verfügbar.";
    return false;
}

bool WasapiDeviceEnumerator::getDefault(
    AudioDeviceFlow, AudioDeviceInfo&, std::string& error) const {
    error = "WASAPI-Geräte sind nur unter Windows verfügbar.";
    return false;
}

} // namespace streamforge

#endif
