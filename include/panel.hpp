#pragma once
#include "audio.hpp"
#include "media.hpp"
#include "endpoint_selection.hpp"
#include <thread>
#include <atomic>
#include <set>
namespace audio {
class Panel {
    Config config_ = ParseConfig(Json::object());
    Audio audio_;
    Media media_;
    service::Provider provider_{[this](const Json& payload){Send(payload);}};
    std::atomic_uint queuedFrames_{};
    Json mediaState_ = {{"status","empty"}};
    bool private_{}, ready_{}, stopping_{};
    std::atomic_bool failed_{};
    int scroll_{};
    HWND window_{};
    std::map<int,HWND> controls_;
    std::vector<std::string> outputs_, inputs_, apps_, players_;
    EndpointSelection output_{false}, input_{true};
    std::string app_, notice_;
    HFONT font_{};
    HBRUSH background_{};
    COLORREF backgroundColor_{},textColor_{},accentColor_{};
    UINT dpi_{96}, taskbarCreated_{};
    HANDLE stop_{};
    std::thread reader_;
    bool tray_{};
    bool soundsPrimed_{}, soundStarted_{};
    std::set<std::string> soundDevices_;
    std::optional<BYTE> lastPower_,lastBattery_;
    ULONGLONG lastCue_{};
    std::vector<int> hotkeys_;
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    void ReadFrames();
    void Frame(const Json& frame);
    void Send(const Json& payload);
    void Tick();
    void RefreshAudio();
    void ResolveSelections(bool available=true);
    std::string StatusNotice() const;
    void Render();
    void Style();
    void Layout();
    void RegisterControls();
    void Settings(Config config);
    void Command(const Json& command);
    void ServiceCommand(const Json& command,const Json& pinned,service::Operation& operation);
    void Action(int id,int notification);
    void Slide(int id);
    void Notice(const std::string& message);
    void Show();
    void Resources();
    void Sounds();
    void Cue(const char* setting);
    void Cleanup();
public:
    explicit Panel(bool privateFixture);
    ~Panel();
    int Run();
};
}
