#include "audio.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>
namespace audio {
std::wstring Wide(const std::string& value) { return winrt::to_hstring(value).c_str(); }
std::string Utf8(std::wstring_view value) { return winrt::to_string(value); }
std::string ProcessName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "";
    wchar_t path[32768]{}; DWORD size = 32768;
    bool ok = QueryFullProcessImageNameW(process,0,path,&size) != FALSE;
    CloseHandle(process);
    return ok ? Utf8(std::filesystem::path(std::wstring(path,size)).filename().wstring()) : "";
}
static std::string Lower(std::string value) { for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return value; }
static std::string Take(LPWSTR value) { if (!value) return ""; auto result = Utf8(value); CoTaskMemFree(value); return result; }
Audio::Audio(DWORD privatePid) : privatePid_(privatePid) {
    winrt::check_hresult(CoCreateGuid(&eventContext_));
    enumerator_ = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
}
Audio::~Audio() { Restore(); }
void Audio::Refresh() {
    endpoints.clear(); sessions.clear(); errors = Json::array();
    for (auto flow : {eRender,eCapture}) {
        std::string defaultId;
        winrt::com_ptr<IMMDevice> primary;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow,eMultimedia,primary.put()))) {
            LPWSTR id{}; if (SUCCEEDED(primary->GetId(&id))) defaultId = Take(id);
        }
        winrt::com_ptr<IMMDeviceCollection> collection;
        const auto result = enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,collection.put());
        if (FAILED(result)) { errors.push_back({{"operation","endpoints"},{"hresult",result}}); continue; }
        UINT count{}; winrt::check_hresult(collection->GetCount(&count));
        for (UINT index=0;index<count;++index) {
            try {
                Endpoint endpoint; endpoint.capture = flow == eCapture;
                winrt::check_hresult(collection->Item(index,endpoint.device.put()));
                LPWSTR id{}; winrt::check_hresult(endpoint.device->GetId(&id)); endpoint.id = Take(id);
                endpoint.isDefault = endpoint.id == defaultId;
                winrt::com_ptr<IPropertyStore> properties;
                winrt::check_hresult(endpoint.device->OpenPropertyStore(STGM_READ,properties.put()));
                PROPVARIANT name; PropVariantInit(&name);
                HRESULT nameResult = properties->GetValue(PKEY_Device_FriendlyName,&name);
                endpoint.name = SUCCEEDED(nameResult) && name.vt == VT_LPWSTR ? Utf8(name.pwszVal) : "Périphérique audio";
                PropVariantClear(&name);
                winrt::check_hresult(endpoint.device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,endpoint.control.put_void()));
                BOOL muted{}; winrt::check_hresult(endpoint.control->GetMute(&muted)); endpoint.muted = muted != FALSE;
                winrt::check_hresult(endpoint.control->GetMasterVolumeLevelScalar(&endpoint.volume));
                winrt::com_ptr<IAudioSessionManager2> manager;
                if (SUCCEEDED(endpoint.device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,manager.put_void()))) {
                    winrt::com_ptr<IAudioSessionEnumerator> list;
                    winrt::check_hresult(manager->GetSessionEnumerator(list.put()));
                    int size{}; winrt::check_hresult(list->GetCount(&size));
                    for (int i=0;i<size;++i) {
                        winrt::com_ptr<IAudioSessionControl> control;
                        if (FAILED(list->GetSession(i,control.put()))) continue;
                        Session session; session.control = control.as<IAudioSessionControl2>();
                        AudioSessionState state{};
                        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) continue;
                        HRESULT pidResult = session.control->GetProcessId(&session.pid);
                        if (FAILED(pidResult)) continue;
                        session.singleProcess = pidResult == S_OK;
                        if (privatePid_ && (session.pid != privatePid_ || !session.singleProcess)) continue;
                        session.active = state == AudioSessionStateActive;
                        LPWSTR instance{}; winrt::check_hresult(session.control->GetSessionInstanceIdentifier(&instance)); session.id = Take(instance);
                        session.endpointId = endpoint.id;
                        session.application = ProcessName(session.pid);
                        LPWSTR display{}; if (SUCCEEDED(control->GetDisplayName(&display))) session.name = Take(display);
                        if (session.name.empty()) session.name = session.application.empty() ? "Sons système / session audio" : session.application;
                        session.mixer = control.as<ISimpleAudioVolume>();
                        winrt::check_hresult(session.mixer->GetMasterVolume(&session.volume));
                        BOOL mute{}; winrt::check_hresult(session.mixer->GetMute(&mute)); session.muted = mute != FALSE;
                        sessions.push_back(std::move(session));
                    }
                }
                endpoints.push_back(std::move(endpoint));
            } catch (const winrt::hresult_error& error) { errors.push_back({{"operation","device"},{"message",winrt::to_string(error.message())}}); }
        }
    }
}
Endpoint& Audio::Device(const std::string& id) {
    auto it = std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& value){return value.id == id;});
    if (it == endpoints.end()) throw std::runtime_error("Périphérique déconnecté. Actualisez le mixeur."); return *it;
}
Session& Audio::App(const std::string& id) {
    auto it = std::find_if(sessions.begin(),sessions.end(),[&](const auto& value){return value.id == id;});
    if (it == sessions.end()) throw std::runtime_error("Session terminée. Actualisez le mixeur."); return *it;
}
void Audio::SetEndpoint(const std::string& id,std::optional<float> volume,std::optional<bool> mute,bool allowMic) {
    if (privatePid_) throw std::runtime_error("Les endpoints sont en lecture seule dans la fixture.");
    auto& endpoint = Device(id);
    if (endpoint.capture && !allowMic) throw std::runtime_error("Autorisez les changements microphone dans les réglages.");
    if (volume) { if (!std::isfinite(*volume) || *volume<0 || *volume>1) throw std::invalid_argument("Volume invalide."); winrt::check_hresult(endpoint.control->SetMasterVolumeLevelScalar(*volume,&eventContext_)); }
    if (mute) winrt::check_hresult(endpoint.control->SetMute(*mute,&eventContext_));
}
void Audio::SetSession(const std::string& id,std::optional<float> volume,std::optional<bool> mute) {
    auto& session = App(id);
    if (Device(session.endpointId).capture) throw std::runtime_error("Le mixeur par application concerne les sorties audio uniquement.");
    if (privatePid_ && (session.pid != privatePid_ || !session.singleProcess)) throw std::runtime_error("Session extérieure à la fixture.");
    // Relinquish automation ownership before an explicit user command.
    if (auto it = owned_.find(id); it != owned_.end()) { it->second.lease.held = false; it->second.suppressed = true; }
    if (volume) { if (!std::isfinite(*volume) || *volume<0 || *volume>1) throw std::invalid_argument("Volume invalide."); winrt::check_hresult(session.mixer->SetMasterVolume(*volume,&eventContext_)); }
    if (mute) winrt::check_hresult(session.mixer->SetMute(*mute,&eventContext_));
}
void Audio::Rule(const Config& config,const std::string& foreground) {
    if (privatePid_) throw std::runtime_error("Les règles de premier plan sont interdites dans la fixture.");
    if (foreground.empty() || !config.flag("enabled") || !config.flag("ruleEnabled") || config.text("ruleApplication").empty() ||
        Lower(foreground) == Lower(config.text("ruleApplication"))) { Restore(); return; }
    std::set<std::string> relevant;
    for (const auto& session : sessions) {
        // Capture sessions and multi-process groups are never modified by a rule.
        if (!session.singleProcess || Lower(session.application) != Lower(config.text("ruleApplication")) || Device(session.endpointId).capture) continue;
        relevant.insert(session.id);
        auto& owned = owned_[session.id]; owned.mixer = session.mixer;
        if (owned.suppressed) continue;
        bool previouslyHeld = owned.lease.held;
        auto changed = owned.lease.Apply(session.volume,config.number("backgroundLimit") / 100.f);
        if (previouslyHeld && !owned.lease.held) owned.suppressed = true;
        if (changed) {
            HRESULT result = session.mixer->SetMasterVolume(*changed,&eventContext_);
            if (FAILED(result)) { owned.lease.held = false; owned.suppressed = true; winrt::check_hresult(result); }
        }
    }
    for (auto it=owned_.begin();it!=owned_.end();) {
        if (!relevant.contains(it->first)) {
            float observed{};
            if (SUCCEEDED(it->second.mixer->GetMasterVolume(&observed)))
                if (auto original=it->second.lease.Restore(observed)) it->second.mixer->SetMasterVolume(*original,&eventContext_);
            it=owned_.erase(it);
        } else ++it;
    }
}
void Audio::Restore() noexcept {
    restoration={};
    for (auto& [id,value] : owned_) {
        float current{};
        if (!value.lease.held) continue;
        ++restoration.attempted;
        if (!value.mixer || FAILED(value.mixer->GetMasterVolume(&current))) {++restoration.failed;continue;}
        if (auto original=value.lease.Restore(current)) {
            if(SUCCEEDED(value.mixer->SetMasterVolume(*original,&eventContext_)))++restoration.restored;else ++restoration.failed;
        } else ++restoration.relinquished;
    }
    owned_.clear();
}
Json Audio::Snapshot() const {
    Json result={{"kind","audio.state"},{"contractVersion",1},{"endpoints",Json::array()},{"sessions",Json::array()},{"errors",errors},{"restoration",{{"attempted",restoration.attempted},{"restored",restoration.restored},{"relinquished",restoration.relinquished},{"failed",restoration.failed}}}};
    for (const auto& endpoint : endpoints) result["endpoints"].push_back({{"id",endpoint.id},{"name",endpoint.name},{"capture",endpoint.capture},{"default",endpoint.isDefault},{"volume",endpoint.volume*100},{"muted",endpoint.muted}});
    for (const auto& session : sessions) result["sessions"].push_back({{"id",session.id},{"endpointId",session.endpointId},{"pid",session.pid},{"name",session.name},{"application",session.application},{"active",session.active},{"volume",session.volume*100},{"muted",session.muted},{"singleProcess",session.singleProcess}});
    return result;
}
}
