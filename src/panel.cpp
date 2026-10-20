#include "panel.hpp"
#include "wire.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <iostream>
#include <mmsystem.h>
#include <filesystem>
#include <fstream>
namespace audio {
namespace {
constexpr UINT frameMessage=WM_APP+1, endMessage=WM_APP+2,trayMessage=WM_APP+3;
enum Id { Output=101,Master,MasterMute,Apps,AppVolume,AppMute,Input,MicVolume,MicMute,Player,Previous,Play,Next,Seek,OpenSound,Status,Track,Title,MasterLabel,AppLabel,MicLabel,MediaLabel };
std::string Selection(HWND combo,const std::vector<std::string>& ids) {
    auto index=SendMessageW(combo,CB_GETCURSEL,0,0); return index>=0 && size_t(index)<ids.size()?ids[size_t(index)]:"";
}
void SetText(HWND window,const std::string& text) { auto wide=Wide(text); SetWindowTextW(window,wide.c_str()); }
COLORREF Color(const std::string& value,COLORREF base) {
    const auto rgba=std::stoul(value.substr(1),nullptr,16); auto a=rgba & 255;
    auto blend=[&](int c,int b){return (c*a+b*(255-a))/255;};
    return RGB(blend((rgba>>24)&255,GetRValue(base)),blend((rgba>>16)&255,GetGValue(base)),blend((rgba>>8)&255,GetBValue(base)));
}
void Combo(HWND window,const std::vector<std::pair<std::string,std::string>>& values,std::vector<std::string>& ids,std::string& selected,bool selectFirst=true) {
    SendMessageW(window,CB_RESETCONTENT,0,0); ids.clear(); int selection=-1;
    for(const auto& [id,label]:values) { if(id==selected)selection=int(ids.size()); ids.push_back(id);auto wide=Wide(label);SendMessageW(window,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(wide.c_str())); }
    if(selectFirst && selection<0 && !ids.empty()) {selection=0;selected=ids.front();}
    if(ids.empty())selected.clear(); SendMessageW(window,CB_SETCURSEL,selection,0);
}
}
Panel::Panel(bool privateFixture):audio_(privateFixture?GetCurrentProcessId():0),private_(privateFixture) {
    stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!private_)provider_.SetExecutor([this](const Json& command,const Json& pinned,service::Operation& op){ServiceCommand(command,pinned,op);});
    WNDCLASSW cls{};cls.lpfnWndProc=Proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MyWallpaperAudioSystemA18";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&cls);
    window_=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Audio média — MyWallpaper",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VSCROLL,0,0,600,760,nullptr,nullptr,cls.hInstance,this);
    if(!stop_||!window_)throw std::runtime_error("Initialisation du panneau impossible.");
    taskbarCreated_=RegisterWindowMessageW(L"TaskbarCreated");dpi_=GetDpiForWindow(window_);
    RegisterControls();Style();Layout();
}
Panel::~Panel() {
    Cleanup(); if(window_)DestroyWindow(window_);if(font_)DeleteObject(font_);if(background_)DeleteObject(background_);
    if(stop_)CloseHandle(stop_);
}
void Panel::RegisterControls() {
    INITCOMMONCONTROLSEX init{sizeof(init),ICC_BAR_CLASSES};InitCommonControlsEx(&init);
    auto make=[&](int id,const wchar_t* type,const wchar_t* text,DWORD style) {
        controls_[id]=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window_,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        if(!controls_[id])throw std::runtime_error("Création d’un contrôle impossible.");
    };
    make(Title,L"STATIC",L"Votre son, vos commandes",SS_LEFT);
    make(MasterLabel,L"STATIC",L"Sortie — le choix ici ne change pas la sortie Windows",SS_LEFT);
    make(Output,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    make(Master,TRACKBAR_CLASSW,L"Volume de la sortie (%)",TBS_HORZ|WS_TABSTOP);
    make(MasterMute,L"BUTTON",L"Couper la sortie",BS_PUSHBUTTON|WS_TABSTOP);
    make(AppLabel,L"STATIC",L"Application / session sur cette sortie",SS_LEFT);
    make(Apps,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    make(AppVolume,TRACKBAR_CLASSW,L"Volume de l’application (%)",TBS_HORZ|WS_TABSTOP);
    make(AppMute,L"BUTTON",L"Couper l’application",BS_PUSHBUTTON|WS_TABSTOP);
    make(MicLabel,L"STATIC",L"Microphone — activité des sessions, aucun enregistrement",SS_LEFT);
    make(Input,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    make(MicVolume,TRACKBAR_CLASSW,L"Volume du microphone (%)",TBS_HORZ|WS_TABSTOP);
    make(MicMute,L"BUTTON",L"Couper le micro",BS_PUSHBUTTON|WS_TABSTOP);
    make(MediaLabel,L"STATIC",L"Lecteur Windows",SS_LEFT);
    make(Player,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    make(Track,L"STATIC",L"Aucune piste publiée par Windows",SS_LEFT|SS_NOPREFIX);
    make(Previous,L"BUTTON",L"Précédent",BS_PUSHBUTTON|WS_TABSTOP);
    make(Play,L"BUTTON",L"Lire / pause",BS_PUSHBUTTON|WS_TABSTOP);
    make(Next,L"BUTTON",L"Suivant",BS_PUSHBUTTON|WS_TABSTOP);
    make(Seek,TRACKBAR_CLASSW,L"Position dans la piste",TBS_HORZ|WS_TABSTOP);
    make(OpenSound,L"BUTTON",L"Réglages audio Windows",BS_PUSHBUTTON|WS_TABSTOP);
    make(Status,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);
    for(auto id:{Master,AppVolume,MicVolume})SendMessageW(controls_[id],TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
    SendMessageW(controls_[Seek],TBM_SETRANGE,TRUE,MAKELPARAM(0,1000));
}
void Panel::Style() {
    HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
    bool high=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
    backgroundColor_=high?GetSysColor(COLOR_WINDOW):Color(config_.text("backgroundColor"),GetSysColor(COLOR_WINDOW));
    textColor_=high?GetSysColor(COLOR_WINDOWTEXT):Color(config_.text("textColor"),backgroundColor_);
    accentColor_=high?GetSysColor(COLOR_HOTLIGHT):Color(config_.text("accentColor"),backgroundColor_);
    if(background_)DeleteObject(background_);background_=CreateSolidBrush(backgroundColor_);
    if(font_)DeleteObject(font_);font_=CreateFontW(-MulDiv(config_.number("textSize"),int(dpi_),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for(const auto& [id,control]:controls_)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    InvalidateRect(window_,nullptr,TRUE);
}
void Panel::Layout() {
    const auto s=[&](int value){return MulDiv(value,int(dpi_),96);};
    MONITORINFO info{sizeof(info)};POINT pointer{};GetCursorPos(&pointer);
    auto monitor=config_.text("monitor")=="pointer"?MonitorFromPoint(pointer,MONITOR_DEFAULTTONEAREST):MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor,&info);
    int width=std::min<int>(s(config_.number("panelWidth")),info.rcWork.right-info.rcWork.left);
    int height=s(705);RECT outer{0,0,width,height};AdjustWindowRectExForDpi(&outer,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VSCROLL,FALSE,WS_EX_CONTROLPARENT,dpi_);
    int w=std::min(outer.right-outer.left,info.rcWork.right-info.rcWork.left),h=std::min(outer.bottom-outer.top,info.rcWork.bottom-info.rcWork.top);
    auto pos=config_.text("position");int margin=s(config_.number("margin"));
    int x=pos=="center"?(info.rcWork.left+info.rcWork.right-w)/2:pos=="bottomLeft"||pos=="topLeft"?info.rcWork.left+margin:info.rcWork.right-w-margin;
    int y=pos=="center"?(info.rcWork.top+info.rcWork.bottom-h)/2:pos=="topLeft"||pos=="topRight"?info.rcWork.top+margin:info.rcWork.bottom-h-margin;
    x=std::clamp<int>(x,info.rcWork.left,info.rcWork.right-w);y=std::clamp<int>(y,info.rcWork.top,info.rcWork.bottom-h);
    SetWindowPos(window_,config_.flag("topmost")?HWND_TOPMOST:HWND_NOTOPMOST,x,y,w,h,SWP_NOACTIVATE);
    RECT client{};GetClientRect(window_,&client);width=MulDiv(client.right,96,int(dpi_));
    auto place=[&](int id,int x0,int y0,int w0,int h0){MoveWindow(controls_[id],s(x0),s(y0)-scroll_,s(w0),s(h0),TRUE);};
    SCROLLINFO scrolling{sizeof(scrolling),SIF_RANGE|SIF_PAGE|SIF_POS,0,s(705),static_cast<UINT>(client.bottom),scroll_,0};
    SetScrollInfo(window_,SB_VERT,&scrolling,TRUE);
    int full=width-40,slider=full-175;
    place(Title,20,12,full,26);place(MasterLabel,20,45,full,24);place(Output,20,72,full,240);
    place(Master,20,108,slider,35);place(MasterMute,width-185,108,165,32);
    place(AppLabel,20,154,full,24);place(Apps,20,181,full,240);
    place(AppVolume,20,217,slider,35);place(AppMute,width-185,217,165,32);
    place(MicLabel,20,263,full,24);place(Input,20,290,full,240);
    place(MicVolume,20,326,slider,35);place(MicMute,width-185,326,165,32);
    place(MediaLabel,20,372,full,24);place(Player,20,399,full,240);place(Track,20,435,full,44);
    place(Previous,20,485,(full-16)/3,32);place(Play,20+(full-16)/3+8,485,(full-16)/3,32);place(Next,20+2*((full-16)/3+8),485,(full-16)/3,32);
    place(Seek,20,522,full,32);place(OpenSound,20,565,full,32);place(Status,20,608,full,80);
    for(int id:{Input,MicVolume,MicMute,MicLabel})ShowWindow(controls_[id],config_.flag("showMicrophone")?SW_SHOW:SW_HIDE);
    for(int id:{MediaLabel,Player,Track,Previous,Play,Next,Seek})ShowWindow(controls_[id],config_.flag("showMedia")?SW_SHOW:SW_HIDE);
}
void Panel::Send(const Json& payload) { wire::Write(std::cout,{{"v",5},{"type","message"},{"target","broadcast"},{"payload",payload}}); }
std::string Panel::StatusNotice() const {
    auto status=output_.Diagnostic();
    auto mic=input_.Diagnostic();if(!mic.empty()) {if(!status.empty())status+=" ";status+=mic;}
    if(!notice_.empty()) {if(!status.empty())status+=" ";status+=notice_;}
    return status;
}
void Panel::Notice(const std::string& message) { notice_=message;SetText(controls_[Status],StatusNotice()); }
void Panel::ReadFrames() {
    try {
        while(WaitForSingleObject(stop_,0)==WAIT_TIMEOUT) {
            auto frame=wire::Read(std::cin);if(!frame)break;
            if(frame->value("type","")=="shutdown")provider_.Revoke();
            provider_.Prepare(*frame);
            if(++queuedFrames_>32)throw std::runtime_error("Native UI frame queue exceeded 32 entries.");
            auto value=std::make_unique<Json>(std::move(*frame));
            if(!PostMessageW(window_,frameMessage,0,reinterpret_cast<LPARAM>(value.get()))) {--queuedFrames_;break;}value.release();
        }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';failed_=true; }
    provider_.Revoke();
    PostMessageW(window_,endMessage,0,0);
}
void Panel::Resources() {
    for(auto id:hotkeys_)UnregisterHotKey(window_,id);hotkeys_.clear();
    if(tray_) { NOTIFYICONDATAW icon{sizeof(icon)};icon.hWnd=window_;icon.uID=1;Shell_NotifyIconW(NIM_DELETE,&icon);tray_=false; }
    if(private_ || !config_.flag("enabled"))return;
    if(config_.flag("trayIcon")) {
        NOTIFYICONDATAW icon{sizeof(icon)};icon.hWnd=window_;icon.uID=1;icon.uFlags=NIF_ICON|NIF_TIP|NIF_MESSAGE;icon.uCallbackMessage=trayMessage;icon.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        wcscpy_s(icon.szTip,L"Mixeur audio MyWallpaper");tray_=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;
        if(!tray_)Notice("L’icône de notification est indisponible. Ouvrez le mixeur depuis les réglages.");
    }
    if(config_.flag("hotkeys")) {
        UINT modifier=MOD_NOREPEAT|MOD_ALT|(config_.text("hotkeyModifier")=="ctrlAlt"?MOD_CONTROL:MOD_WIN);
        int index=0;for(auto key:{VK_UP,VK_DOWN,int('M'),int('A')}) { int id=800+index++;if(RegisterHotKey(window_,id,modifier,key))hotkeys_.push_back(id);else Notice("Un raccourci audio est déjà utilisé. Les commandes du panneau restent disponibles."); }
    }
}
void Panel::Cue(const char* setting) {
    if(private_ || !config_.flag("enabled") || !config_.flag("soundsEnabled"))return;
    const auto value=config_.text(setting);if(value.empty())return;
    const auto now=GetTickCount64();if(lastCue_ && now-lastCue_<1000)return;
    std::filesystem::path path(Wide(value));auto extension=path.extension().wstring();for(auto& c:extension)c=static_cast<wchar_t>(towlower(c));
    if(!path.is_absolute() || value.rfind("\\\\",0)==0 || extension!=L".wav" || !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path)>8*1024*1024)throw std::runtime_error("Son indisponible : choisissez un fichier WAV local de 8 MiB maximum.");
    std::ifstream file(path,std::ios::binary);char header[12]{};file.read(header,12);
    if(file.gcount()!=12 || std::string(header,4)!="RIFF" || std::string(header+8,4)!="WAVE")throw std::runtime_error("Le fichier choisi n’est pas un WAV RIFF.");
    if(!PlaySoundW(path.c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_NODEFAULT|SND_NOSTOP))throw std::runtime_error("Windows n’a pas pu lire le son choisi.");
    lastCue_=now;soundStarted_=true;
}
void Panel::Sounds() {
    if(private_ || !config_.flag("soundsEnabled"))return;
    std::set<std::string> devices;for(const auto& endpoint:audio_.endpoints)devices.insert(endpoint.id);
    SYSTEM_POWER_STATUS power{};bool available=GetSystemPowerStatus(&power)!=FALSE;
    const bool connected=soundsPrimed_ && std::any_of(devices.begin(),devices.end(),[&](const auto& id){return !soundDevices_.contains(id);});
    const bool powerChanged=soundsPrimed_ && available && power.ACLineStatus!=255 && lastPower_ && *lastPower_!=power.ACLineStatus;
    const bool batteryLow=soundsPrimed_ && available && power.BatteryLifePercent!=255 && lastBattery_ && *lastBattery_>config_.number("lowBattery") && power.BatteryLifePercent<=config_.number("lowBattery");
    soundDevices_=std::move(devices);soundsPrimed_=true;
    if(available && power.ACLineStatus!=255)lastPower_=power.ACLineStatus;
    if(available && power.BatteryLifePercent!=255)lastBattery_=power.BatteryLifePercent;
    // One cue per observation: battery has priority, then power, then device.
    if(batteryLow)Cue("batterySound");else if(powerChanged)Cue("powerSound");else if(connected)Cue("deviceSound");
}
void Panel::Settings(Config config) {
    audio_.Restore();
    if(soundStarted_){PlaySoundW(nullptr,nullptr,0);soundStarted_=false;}soundsPrimed_=false;lastPower_.reset();lastBattery_.reset();
    config_=std::move(config);ResolveSelections();Style();Layout();Resources();
    KillTimer(window_,1);if(config_.flag("enabled"))SetTimer(window_,1,config_.number("refreshMs"),nullptr);
    if(!private_ && config_.flag("enabled") && config_.flag("showPanel"))Show();else ShowWindow(window_,SW_HIDE);
}
void Panel::Show() { if(!private_ && config_.flag("enabled"))ShowWindow(window_,SW_SHOWNOACTIVATE); }
void Panel::Frame(const Json& frame) {
    if(frame.at("v")!=5)throw std::invalid_argument("Version process-v2 attendue : 5.");
    const auto type=Text(frame,"type",32);
    if(!ready_) {
        if(type!="init")throw std::invalid_argument("Initialisation process-v2 attendue.");
        auto config=ParseConfig(frame.at("deviceSettings"));
        // ready precedes all component state and errors.
        wire::Write(std::cout,{{"v",5},{"type","ready"}});ready_=true;Settings(std::move(config));Tick();return;
    }
    if(type=="shutdown") {Cleanup();PostQuitMessage(0);return;}
    if(type=="settings") {Settings(ParseConfig(frame.at("deviceSettings")));Send({{"kind","audio.settings"},{"ok",true}});Tick();return;}
    if(type!="message")throw std::invalid_argument("Type de message inconnu.");
    if(provider_.Receive(frame)) {
        const auto& payload=frame.at("payload");
        if(payload.value("kind","")=="open" || (payload.contains("packet")&&payload.at("packet").value("method","")=="command"))Tick();
        return;
    }
    const auto& payload=frame.at("payload");const auto kind=Text(payload,"kind",64);
    auto request=payload.value("requestId",Json(nullptr));
    if(!request.is_null() && (!request.is_string() || request.get_ref<const std::string&>().size()>256))throw std::invalid_argument("Identifiant de requête invalide.");
    std::string correlation;
    if(payload.contains("correlationId")) {
        const auto& value=payload.at("correlationId");
        if(!value.is_string())throw std::invalid_argument("Identifiant de corrélation invalide.");
        correlation=value.get<std::string>();
        if(correlation.empty() || correlation.size()>128 || correlation.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=std::string::npos)
            throw std::invalid_argument("Identifiant de corrélation invalide.");
    }
    auto respond=[&](Json result){if(!correlation.empty())result["correlationId"]=correlation;Send(result);};
    try {
        if(!config_.flag("enabled"))throw std::runtime_error("Les commandes audio sont désactivées.");
        if(kind=="media.command") {if(private_)throw std::runtime_error("Média extérieur interdit dans la fixture.");media_.Command(payload);}
        else if(kind=="audio.command")Command(payload);else throw std::invalid_argument("Contrat audio/média inconnu.");
        respond({{"kind",kind=="media.command"?"media.result":"audio.result"},{"requestId",request},{"ok",true}});
    } catch(const winrt::hresult_error& error) { auto message=winrt::to_string(error.message());Notice(message);respond({{"kind",kind=="media.command"?"media.result":"audio.result"},{"requestId",request},{"ok",false},{"message",message}}); }
      catch(const std::exception& error) {Notice(error.what());respond({{"kind",kind=="media.command"?"media.result":"audio.result"},{"requestId",request},{"ok",false},{"message",error.what()}});}
    Tick();
}
void Panel::Command(const Json& command) {
    const auto action=Text(command,"action",64);
    if(action=="refresh")return;
    if(action=="show") {Show();return;}
    auto id=Text(command,"id");
    if(action=="endpointVolume")audio_.SetEndpoint(id,Percent(command),{},config_.flag("allowMicrophoneChanges"));
    else if(action=="endpointMute")audio_.SetEndpoint(id,{},command.at("muted").get<bool>(),config_.flag("allowMicrophoneChanges"));
    else if(action=="sessionVolume")audio_.SetSession(id,Percent(command),{});
    else if(action=="sessionMute")audio_.SetSession(id,{},command.at("muted").get<bool>());
    else throw std::invalid_argument("Commande audio inconnue.");
    if(action=="endpointMute"||action=="sessionMute"){try{Cue("interactionSound");}catch(const std::exception& error){Notice(error.what());}}
}
void Panel::ServiceCommand(const Json& command,const Json& pinned,service::Operation& operation) try {
    operation.Check();
    if(private_||!config_.flag("enabled"))throw service::Failure("permission_denied","Audio commands are disabled");
    if(command.at("kind")=="media.command") {
        if(!config_.flag("showMedia"))throw service::Failure("unavailable","Media collection is disabled");
        media_.Command(command,pinned.is_null()?std::string{}:pinned.get<std::string>(),&operation);return;
    }
    const auto action=command.at("action").get<std::string>();
    if(action=="refresh") {operation.Dispatch();RefreshAudio();return;}
    if(action=="show") {operation.Dispatch();Show();return;}
    // Re-enumerate only for an explicit authorized command, then check before
    // entering any setter. No cached/foreign ID is used as a fallback.
    RefreshAudio();operation.Check();
    const auto id=command.at("id").get<std::string>();
    const bool endpoint=action.rfind("endpoint",0)==0;
    if(endpoint) {
        if(audio_.Device(id).capture&&!config_.flag("allowMicrophoneChanges"))throw service::Failure("permission_denied","Microphone writes are disabled");
    } else if(audio_.Device(audio_.App(id).endpointId).capture)throw service::Failure("permission_denied","Capture sessions are read only");
    operation.Dispatch();
    Command(command);
} catch(const winrt::hresult_error& error) {
    throw service::Failure("native_failure",winrt::to_string(error.message()));
}
void Panel::ResolveSelections(bool available) {
    std::vector<EndpointOption> inventory;
    if(available)for(const auto& endpoint:audio_.endpoints)inventory.push_back({endpoint.id,endpoint.name,endpoint.capture,endpoint.isDefault});
    const auto previous=output_.Id();
    output_.Refresh(inventory,config_.text("preferredOutput"));
    input_.Refresh(inventory,config_.text("preferredMicrophone"));
    // Sessions belong to the resolved output. A dropped-down list may still
    // show old entries, but none of their IDs may survive an inventory change.
    if(output_.Id().empty() || previous!=output_.Id() ||
       std::none_of(audio_.sessions.begin(),audio_.sessions.end(),[&](const auto& session) {
           return session.id==app_ && session.endpointId==output_.Id() && (session.active || config_.flag("showInactive"));
       }))app_.clear();
}
void Panel::RefreshAudio() {
    try {audio_.Refresh();}
    catch(...) {ResolveSelections(false);Render();throw;}
    ResolveSelections();
}
void Panel::Tick() {
    if(!ready_)return;
    if(!config_.flag("enabled")) {provider_.AudioState(Json::object(),false,false);provider_.MediaState(Json::object(),false,false);return;}
    try {RefreshAudio();}catch(const std::exception& e){provider_.Unavailable("audio",e.what());throw;}
      catch(const winrt::hresult_error& e){provider_.Unavailable("audio",winrt::to_string(e.message()));throw;}
    try {Sounds();}catch(const std::exception& error){Notice(error.what());}
    if(!private_ && config_.flag("ruleEnabled")) {
        DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);audio_.Rule(config_,ProcessName(pid));
    }
    auto state=audio_.Snapshot();
    state["runtime"]={{"privateFixture",private_},{"pid",GetCurrentProcessId()},{"hwnd",reinterpret_cast<uintptr_t>(window_)},{"visible",IsWindowVisible(window_)!=FALSE},{"hotkeys",hotkeys_.size()},{"tray",tray_},{"soundStarted",soundStarted_},{"notice",StatusNotice()}};
    provider_.AudioState(state,true,config_.flag("allowMicrophoneChanges"));
    Send(state);
    if(!private_ && config_.flag("showMedia")) {
        try {mediaState_=media_.Snapshot();}
        catch(const winrt::hresult_error& e){mediaState_={{"kind","media.state"},{"status","unavailable"},{"message",winrt::to_string(e.message())}};}
        catch(const std::exception& e){mediaState_={{"kind","media.state"},{"status","unavailable"},{"message",e.what()}};}
        provider_.MediaState(mediaState_,true,true);
        Send(mediaState_);
    }
    if(private_ || !config_.flag("showMedia"))provider_.MediaState(Json::object(),true,false);
    Render();
}
void Panel::Render() {
    // Avoid rebuilding an open native drop-down while keyboard navigation is in progress.
    bool dropdownOpen=false;
    for(int id:{Output,Apps,Input,Player})if(SendMessageW(controls_[id],CB_GETDROPPEDSTATE,0,0))dropdownOpen=true;
    std::vector<std::pair<std::string,std::string>> outputs,inputs,apps,players;
    for(const auto& endpoint:audio_.endpoints) {
        auto label=endpoint.name+(endpoint.isDefault?" (Windows)":"");
        (endpoint.capture?inputs:outputs).emplace_back(endpoint.id,label);
    }
    if(!dropdownOpen) {
        auto output=output_.Id(),input=input_.Id();
        Combo(controls_[Output],outputs,outputs_,output,false);Combo(controls_[Input],inputs,inputs_,input,false);
    }
    int activeMic=0;
    for(const auto& session:audio_.sessions) {
        if(session.endpointId==input_.Id() && session.active)++activeMic;
        if(session.endpointId==output_.Id() && (session.active || config_.flag("showInactive")))apps.emplace_back(session.id,session.name+(session.active?" • actif":" • silencieux"));
    }
    if(!dropdownOpen)Combo(controls_[Apps],apps,apps_,app_);
    auto slider=[&](int id,float volume) {if(GetFocus()!=controls_[id])SendMessageW(controls_[id],TBM_SETPOS,TRUE,int(volume*100+.5f));};
    for(auto [id,selected]:{std::pair{Master,output_.Id()},std::pair{MicVolume,input_.Id()}}) {
        bool available=!selected.empty();EnableWindow(controls_[id],available && (id!=MicVolume || config_.flag("allowMicrophoneChanges")));
        EnableWindow(controls_[id==Master?MasterMute:MicMute],available && (id!=MicVolume || config_.flag("allowMicrophoneChanges")));
        if(available) {auto& endpoint=audio_.Device(selected);slider(id,endpoint.volume);SetText(controls_[id==Master?MasterMute:MicMute],std::string(endpoint.muted?"Rétablir ":"Couper ")+(id==Master?"la sortie":"le micro"));}
    }
    EnableWindow(controls_[AppVolume],!app_.empty());EnableWindow(controls_[AppMute],!app_.empty());
    if(!app_.empty()){auto& session=audio_.App(app_);slider(AppVolume,session.volume);SetText(controls_[AppMute],session.muted?"Rétablir l’application":"Couper l’application");}
    if(mediaState_.contains("sessions"))for(const auto& player:mediaState_.at("sessions"))players.emplace_back(player.at("id").get<std::string>(),player.at("label").get<std::string>());
    std::string selectedPlayer=mediaState_.value("sessionId",std::string{});if(!dropdownOpen)Combo(controls_[Player],players,players_,selectedPlayer);
    std::string track=mediaState_.value("title",std::string{});auto artist=mediaState_.value("artist",std::string{});if(!artist.empty())track+=" — "+artist;
    if(track.empty())track=mediaState_.value("status",std::string{})=="unavailable"?"Média indisponible : "+mediaState_.value("message",std::string{}):"Aucune piste publiée par Windows";
    SetText(controls_[Track],track);SetText(controls_[Play],mediaState_.value("playing",false)?"Pause":"Lire");
    auto caps=mediaState_.value("capabilities",Json::object());
    for(auto [id,key]:{std::pair{Previous,"previous"},std::pair{Play,"playPause"},std::pair{Next,"next"},std::pair{Seek,"seek"}})EnableWindow(controls_[id],caps.value(key,false));
    auto duration=mediaState_.value("durationMs",int64_t{}),position=mediaState_.value("positionMs",int64_t{});
    if(GetFocus()!=controls_[Seek])SendMessageW(controls_[Seek],TBM_SETPOS,TRUE,duration>0?std::clamp(int(position*1000/duration),0,1000):0);
    auto status=std::string(config_.flag("showMicrophone")?"Micro : "+std::to_string(activeMic)+" session(s) active(s). ":"")+StatusNotice();
    if(!audio_.errors.empty())status+=" Certaines sessions sont indisponibles (détails audio.state).";
    SetText(controls_[Status],status);
}
void Panel::Action(int id,int notification) {
    if(!config_.flag("enabled") || private_)return;
    if(notification==CBN_SELCHANGE) {
        if(id==Output){output_.Choose(Selection(controls_[Output],outputs_));app_.clear();}
        if(id==Input)input_.Choose(Selection(controls_[Input],inputs_));
        if(id==Apps)app_=Selection(controls_[Apps],apps_);
        ResolveSelections();
        if(id==Player)media_.Command({{"action","session"},{"value",Selection(controls_[Player],players_)}});
    } else if(notification==BN_CLICKED) {
        if(id==MasterMute && !output_.Id().empty())audio_.SetEndpoint(output_.Id(),{},!audio_.Device(output_.Id()).muted,false);
        if(id==MicMute && !input_.Id().empty())audio_.SetEndpoint(input_.Id(),{},!audio_.Device(input_.Id()).muted,config_.flag("allowMicrophoneChanges"));
        if(id==AppMute && !app_.empty())audio_.SetSession(app_,{},!audio_.App(app_).muted);
        if(id==Previous||id==Play||id==Next)media_.Command({{"action",id==Previous?"previous":id==Next?"next":"playPause"}});
        if(id==MasterMute||id==MicMute||id==AppMute)Cue("interactionSound");
        if(id==OpenSound) {auto result=ShellExecuteW(window_,L"open",L"ms-settings:sound",nullptr,nullptr,SW_SHOWNORMAL);if(INT_PTR(result)<=32)throw std::runtime_error("Impossible d’ouvrir les réglages audio Windows.");}
    }
    Tick();
}
void Panel::Slide(int id) {
    if(private_||!config_.flag("enabled"))return;
    float volume=float(SendMessageW(controls_[id],TBM_GETPOS,0,0))/100.f;
    if(id==Master && !output_.Id().empty())audio_.SetEndpoint(output_.Id(),volume,{},false);
    if(id==MicVolume && !input_.Id().empty())audio_.SetEndpoint(input_.Id(),volume,{},config_.flag("allowMicrophoneChanges"));
    if(id==AppVolume && !app_.empty())audio_.SetSession(app_,volume,{});
    if(id==Seek) {
        auto duration=mediaState_.value("durationMs",int64_t{});
        int64_t position=duration*SendMessageW(controls_[Seek],TBM_GETPOS,0,0)/1000;
        media_.Command({{"action","seek"},{"value",position}});
    }
}
void Panel::Cleanup() {
    if(stopping_)return;stopping_=true;provider_.Revoke();
    try{provider_.Close("Native provider shutdown");}catch(...){}
    KillTimer(window_,1);
    for(auto id:hotkeys_)UnregisterHotKey(window_,id);hotkeys_.clear();
    if(tray_){NOTIFYICONDATAW icon{sizeof(icon)};icon.hWnd=window_;icon.uID=1;Shell_NotifyIconW(NIM_DELETE,&icon);tray_=false;}
    if(soundStarted_){PlaySoundW(nullptr,nullptr,0);soundStarted_=false;}
    audio_.Restore();
    if(ready_){try{Send({{"kind","audio.lifecycle"},{"event","stopped"},{"restoration",{{"attempted",audio_.restoration.attempted},{"restored",audio_.restoration.restored},{"relinquished",audio_.restoration.relinquished},{"failed",audio_.restoration.failed}}}});}catch(...){}}
    ShowWindow(window_,SW_HIDE);SetEvent(stop_);
    if(reader_.joinable()){CancelSynchronousIo(reader_.native_handle());reader_.join();}
    MSG pending{};while(PeekMessageW(&pending,window_,frameMessage,frameMessage,PM_REMOVE)){delete reinterpret_cast<Json*>(pending.lParam);--queuedFrames_;}
}
int Panel::Run() {
    reader_=std::thread([this]{ReadFrames();});MSG message{};
    HWND previousFocus=nullptr;
    while(GetMessageW(&message,nullptr,0,0)>0) {
        if(!IsDialogMessageW(window_,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        auto focus=GetFocus();
        if(!private_ && focus && focus!=previousFocus && IsChild(window_,focus)) {
            RECT rect{},client{};GetWindowRect(focus,&rect);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&rect),2);GetClientRect(window_,&client);
            int next=scroll_;
            if(rect.top<0)next+=rect.top;else if(rect.bottom>client.bottom)next+=rect.bottom-client.bottom;
            SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window_,SB_VERT,&info);
            next=std::clamp(next,0,std::max(0,info.nMax-int(info.nPage)+1));
            if(next!=scroll_){scroll_=next;Layout();}
        }
        previousFocus=focus;
    }
    Cleanup();return failed_?1:0;
}
LRESULT CALLBACK Panel::Proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto self=reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(window,message,wparam,lparam);
    try {
        if(message==frameMessage){std::unique_ptr<Json> frame(reinterpret_cast<Json*>(lparam));--self->queuedFrames_;self->Frame(*frame);return 0;}
        if(message==endMessage){self->Cleanup();PostQuitMessage(0);return 0;}
        if(message==WM_TIMER){self->Tick();return 0;}
        if(message==WM_CLOSE){ShowWindow(window,SW_HIDE);return 0;}
        if(message==WM_COMMAND){self->Action(LOWORD(wparam),HIWORD(wparam));return 0;}
        if(message==WM_HSCROLL){int id=GetDlgCtrlID(reinterpret_cast<HWND>(lparam));if(id!=Seek || LOWORD(wparam)!=TB_THUMBTRACK)self->Slide(id);return 0;}
        if(message==WM_MOUSEWHEEL && !self->private_ && self->config_.flag("enabled") && !self->output_.Id().empty()) {
            auto& device=self->audio_.Device(self->output_.Id());float delta=float(GET_WHEEL_DELTA_WPARAM(wparam))/WHEEL_DELTA*self->config_.number("volumeStep")/100;
            self->audio_.SetEndpoint(self->output_.Id(),std::clamp(device.volume+delta,0.f,1.f),{},false);self->Tick();return 0;
        }
        if(message==WM_HOTKEY && !self->private_ && self->config_.flag("enabled")) {
            if(wparam==803)self->Show();
            else if(!self->output_.Id().empty()) {auto& device=self->audio_.Device(self->output_.Id());if(wparam==802){self->audio_.SetEndpoint(self->output_.Id(),{},!device.muted,false);self->Cue("interactionSound");}else self->audio_.SetEndpoint(self->output_.Id(),std::clamp(device.volume+(wparam==800?1:-1)*self->config_.number("volumeStep")/100.f,0.f,1.f),{},false);self->Tick();}return 0;
        }
        if(message==trayMessage){if(lparam==WM_LBUTTONUP||lparam==WM_RBUTTONUP)self->Show();return 0;}
        if(message==self->taskbarCreated_ && self->ready_){self->tray_=false;self->Resources();return 0;}
        if(message==WM_VSCROLL) {
            SCROLLINFO scrolling{sizeof(scrolling),SIF_ALL};GetScrollInfo(window,SB_VERT,&scrolling);
            int next=self->scroll_;switch(LOWORD(wparam)){case SB_LINEUP:next-=24;break;case SB_LINEDOWN:next+=24;break;case SB_PAGEUP:next-=int(scrolling.nPage);break;case SB_PAGEDOWN:next+=int(scrolling.nPage);break;case SB_THUMBTRACK:next=scrolling.nTrackPos;break;}
            self->scroll_=std::clamp(next,0,std::max(0,scrolling.nMax-int(scrolling.nPage)+1));self->Layout();return 0;
        }
        if(message==WM_DPICHANGED){self->dpi_=HIWORD(wparam);self->Style();self->Layout();return 0;}
        if(message==WM_DISPLAYCHANGE){self->Layout();return 0;}
        if(message==WM_SETTINGCHANGE){self->Style();return 0;}
        if(message==WM_ERASEBKGND){RECT rect{};GetClientRect(window,&rect);FillRect(reinterpret_cast<HDC>(wparam),&rect,self->background_);return 1;}
        if(message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX) {
            HDC dc=reinterpret_cast<HDC>(wparam);SetBkColor(dc,self->backgroundColor_);SetTextColor(dc,reinterpret_cast<HWND>(lparam)==self->controls_[Title]?self->accentColor_:self->textColor_);return reinterpret_cast<LRESULT>(self->background_);
        }
        if(message==WM_QUERYENDSESSION)return TRUE;
        if(message==WM_ENDSESSION&&wparam){self->Cleanup();return 0;}
    } catch(const winrt::hresult_error& error) {self->Notice(winrt::to_string(error.message()));}
      catch(const std::exception& error) {
        self->Notice(error.what());
        if(message==frameMessage) {
            if(!self->ready_){wire::Write(std::cout,{{"v",5},{"type","error"},{"code","invalid_init"},{"message",error.what()}});self->failed_=true;PostQuitMessage(1);}
            else self->Send({{"kind","audio.error"},{"message",error.what()}});
        }
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
