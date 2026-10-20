#pragma once
#include "model.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <winrt/base.h>
#include <map>
namespace audio {
std::wstring Wide(const std::string& value);
std::string Utf8(std::wstring_view value);
std::string ProcessName(DWORD pid);
struct Endpoint {
    std::string id, name;
    bool capture{}, isDefault{}, muted{};
    float volume{};
    winrt::com_ptr<IMMDevice> device;
    winrt::com_ptr<IAudioEndpointVolume> control;
};
struct Session {
    std::string id, endpointId, name, application;
    DWORD pid{};
    bool active{}, muted{}, singleProcess{};
    float volume{};
    winrt::com_ptr<IAudioSessionControl2> control;
    winrt::com_ptr<ISimpleAudioVolume> mixer;
};
struct Restoration {unsigned attempted{}, restored{}, relinquished{}, failed{};};
class Audio {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator_;
    DWORD privatePid_{};
    struct Owned { Lease lease; bool suppressed{}; winrt::com_ptr<ISimpleAudioVolume> mixer; };
    std::map<std::string,Owned> owned_;
    GUID eventContext_{};
public:
    std::vector<Endpoint> endpoints;
    std::vector<Session> sessions;
    Json errors = Json::array();
    Restoration restoration;
    explicit Audio(DWORD privatePid = 0);
    ~Audio();
    void Refresh();
    Endpoint& Device(const std::string& id);
    Session& App(const std::string& id);
    void SetEndpoint(const std::string& id, std::optional<float> volume, std::optional<bool> mute, bool allowMic);
    void SetSession(const std::string& id, std::optional<float> volume, std::optional<bool> mute);
    void Rule(const Config& config, const std::string& foreground);
    void Restore() noexcept;
    Json Snapshot() const;
};
}
