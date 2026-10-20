#include "wire.hpp"
#include "audio.hpp"
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <set>
#include <cmath>
#include <algorithm>
#include <vector>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include "service_provider.hpp"
using namespace audio;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Child {
    HANDLE process{},input{},output{};
    Child(const wchar_t* executable) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE readIn{},writeOut{};
        Require(CreatePipe(&readIn,&input,&security,0)!=FALSE,"stdin pipe");
        Require(CreatePipe(&output,&writeOut,&security,0)!=FALSE,"stdout pipe");
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESTDHANDLES;start.hStdInput=readIn;start.hStdOutput=writeOut;start.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION info{};std::wstring command=L"\""+std::wstring(executable)+L"\" --private-fixture";
        BOOL ok=CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&info);
        CloseHandle(readIn);CloseHandle(writeOut);Require(ok!=FALSE,"private companion launch");process=info.hProcess;CloseHandle(info.hThread);
    }
    ~Child(){if(process){if(WaitForSingleObject(process,0)==WAIT_TIMEOUT)TerminateProcess(process,99);CloseHandle(process);}if(input)CloseHandle(input);if(output)CloseHandle(output);}
    void Send(const Json& value){std::ostringstream bytes;wire::Write(bytes,value);auto data=bytes.str();DWORD written{};Require(WriteFile(input,data.data(),DWORD(data.size()),&written,nullptr)&&written==data.size(),"process write");}
    void ReadExact(char* data,DWORD size) {
        DWORD total=0;auto deadline=GetTickCount64()+5000;
        while(total<size) {
            DWORD available{};Require(PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)!=FALSE,"output closed prematurely");
            if(!available){Require(GetTickCount64()<deadline,"private process response timeout");Sleep(5);continue;}
            DWORD read{};Require(ReadFile(output,data+total,std::min(size-total,available),&read,nullptr)&&read>0,"process read");total+=read;
        }
    }
    Json Read() {
        unsigned char header[4]{};ReadExact(reinterpret_cast<char*>(header),4);
        auto size=uint32_t(header[0])|uint32_t(header[1])<<8|uint32_t(header[2])<<16|uint32_t(header[3])<<24;
        Require(size>0&&size<1024*1024,"fixture expects one ordinary output record");std::string data(size,'\0');ReadExact(data.data(),size);return Json::parse(data);
    }
    Json Payload(const std::string& kind) {for(int i=0;i<12;++i){auto frame=Read();if(frame.value("type","")=="message"&&frame.at("payload").value("kind","")==kind)return frame.at("payload");}throw std::runtime_error("expected payload missing");}
    void Exit(DWORD expected) {Require(WaitForSingleObject(process,5000)==WAIT_OBJECT_0,"private process did not stop");DWORD code{};GetExitCodeProcess(process,&code);Require(code==expected,"private process exit code");}
};

// Explicit opt-in observation branch; never constructs Child or sends wire frames.
static unsigned long long Decimal(const wchar_t* text) {
    const std::wstring value(text);Require(!value.empty() && value.size()<=20 && value.find_first_not_of(L"0123456789")==std::wstring::npos,"Exact unsigned decimal required");
    size_t end=0;const auto number=std::stoull(value,&end);Require(end==value.size() && number>0,"Nonzero identity required");return number;
}
static unsigned long long Number(FILETIME value) {return (static_cast<unsigned long long>(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
static std::string ImageHash(const wchar_t* value) {
    const std::string hash=Utf8(value);
    Require(hash.size()==71 && hash.rfind("sha256:",0)==0 && hash.substr(7).find_first_not_of("0123456789abcdef")==std::string::npos,"Exact lowercase image SHA256 required");
    return hash;
}
static void MatchIdentity(unsigned long long actualBirth,unsigned long long expectedBirth,bool live,const std::string& actualHash,const std::string& expectedHash) {
    Require(live && actualBirth==expectedBirth,"Process generation not live or differs");
    Require(actualHash==expectedHash,"Image hash differs");
}
static void IdentityBounds(unsigned long long pid,unsigned long long hwnd) {
    Require(pid>0 && pid<=MAXDWORD && hwnd>0 && hwnd<=UINTPTR_MAX,"Identity outside native bounds");
}
static void RootFacts(DWORD expectedPid,DWORD owner,DWORD thread,bool root,bool visible,const std::wstring& name) {
    Require(owner==expectedPid && thread!=0 && root && visible && name==L"MyWallpaperAudioSystemA18","Visible window owner/class differs");
}
// Every required API result is recorded. A failed stage leaves later fields
// unobserved, never silently successful. Coverage is only this declared subset.
struct InventoryCoverage {
    bool complete{true}; Json calls=Json::array();
    void Check(const char* operation,HRESULT hr,bool valid=true,int index=-1) {
        const bool ok=SUCCEEDED(hr)&&valid;
        calls.push_back({{"operation",operation},{"index",index},{"hresult",static_cast<long>(hr)},{"valueValid",valid},{"ok",ok}});
        if(!ok){complete=false;throw std::runtime_error(operation);}
    }
    Json Result() const {return {{"scope","active-render-endpoints/eMultimedia"},{"complete",complete},{"requiredCalls",calls},{"failurePolicy","remaining dependent calls unobserved on failed stage"},{"excluded",{"capture endpoints","audio sessions","provider envelope/generation","exhaustive OS audio"}}};}
};
static Json ReadRenderInventory() {
    InventoryCoverage coverage;Json rows=Json::array();std::string defaultId;UINT count=0;
    HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Apartment {bool acquired;~Apartment(){if(acquired)CoUninitialize();}} apartmentGuard{SUCCEEDED(apartment)};
    try {
        coverage.Check("CoInitializeEx",apartment);
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),enumerator.put_void());
        coverage.Check("CoCreateInstance(IMMDeviceEnumerator)",hr,enumerator!=nullptr);
        winrt::com_ptr<IMMDevice> primary;
        hr=enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,primary.put());coverage.Check("GetDefaultAudioEndpoint(eRender,eMultimedia)",hr,primary!=nullptr);
        LPWSTR primaryId=nullptr;hr=primary->GetId(&primaryId);
        std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> primaryIdGuard(primaryId,CoTaskMemFree);
        coverage.Check("default.GetId",hr,primaryId && *primaryId);defaultId=Utf8(primaryId);
        winrt::com_ptr<IMMDeviceCollection> devices;
        hr=enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,devices.put());coverage.Check("EnumAudioEndpoints(eRender,ACTIVE)",hr,devices!=nullptr);
        hr=devices->GetCount(&count);coverage.Check("collection.GetCount",hr,count<=4096);
        std::set<std::string> ids;
        for(UINT i=0;i<count;++i) {
            Json row={{"index",i},{"complete",false}};
            try {
                winrt::com_ptr<IMMDevice> device;hr=devices->Item(i,device.put());coverage.Check("collection.Item",hr,device!=nullptr,i);
                LPWSTR rawId=nullptr;hr=device->GetId(&rawId);std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> idGuard(rawId,CoTaskMemFree);
                coverage.Check("endpoint.GetId",hr,rawId && *rawId,i);const auto id=Utf8(rawId);
                coverage.Check("endpoint.uniqueId",S_OK,ids.insert(id).second,i);row["id"]=id;
                DWORD state=0;hr=device->GetState(&state);coverage.Check("endpoint.GetState",hr,state==DEVICE_STATE_ACTIVE,i);
                winrt::com_ptr<IPropertyStore> properties;hr=device->OpenPropertyStore(STGM_READ,properties.put());coverage.Check("endpoint.OpenPropertyStore(READ)",hr,properties!=nullptr,i);
                PROPVARIANT name{};PropVariantInit(&name);
                struct Variant {PROPVARIANT* p;~Variant(){PropVariantClear(p);}} nameGuard{&name};
                hr=properties->GetValue(PKEY_Device_FriendlyName,&name);coverage.Check("properties.GetValue(FriendlyName)",hr,name.vt==VT_LPWSTR && name.pwszVal && *name.pwszVal,i);
                row["name"]=Utf8(name.pwszVal);row["default"]=id==defaultId;row["capture"]=false;
                winrt::com_ptr<IAudioEndpointVolume> volume;hr=device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,volume.put_void());coverage.Check("endpoint.Activate(IAudioEndpointVolume)",hr,volume!=nullptr,i);
                BOOL muted=FALSE;hr=volume->GetMute(&muted);coverage.Check("volume.GetMute",hr,muted==FALSE || muted==TRUE,i);
                float scalar=0;hr=volume->GetMasterVolumeLevelScalar(&scalar);coverage.Check("volume.GetMasterVolumeLevelScalar",hr,std::isfinite(scalar)&&scalar>=0&&scalar<=1,i);
                row["muted"]=muted!=FALSE;row["volume"]=scalar*100;row["complete"]=true;
            } catch(const std::exception& error) {coverage.complete=false;row["failedStage"]=error.what();}
            rows.push_back(std::move(row));
        }
        coverage.Check("default.presentInActiveRenderInventory",S_OK,ids.contains(defaultId));
    } catch(const std::exception& error) {coverage.complete=false;coverage.calls.push_back({{"terminatedStage",error.what()}});}
    return {{"coverage",coverage.Result()},{"defaultId",defaultId},{"enumeratedCount",count},{"endpoints",rows}};
}
static Json ReaderSelfTest() {
    unsigned checks=0;
    auto rejects=[&](auto action){bool refused=false;try{action();}catch(const std::exception&){refused=true;}Require(refused,"expected refusal");++checks;};
    Require(Decimal(L"123")==123,"decimal positive");++checks;
    for(const auto value:{L"",L"0",L"-1",L"+1",L"1x",L"18446744073709551616"})rejects([&]{Decimal(value);});
    const std::wstring hash=L"sha256:"+std::wstring(64,L'a');const auto good=ImageHash(hash.c_str());++checks;
    for(const auto value:{L"sha256:abc",L"sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",L"other:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})rejects([&]{ImageHash(value);});
    IdentityBounds(123,123);++checks;
    rejects([&]{IdentityBounds(0,123);});rejects([&]{IdentityBounds(0x100000000ULL,123);});rejects([&]{IdentityBounds(123,0);});
    RootFacts(123,123,456,true,true,L"MyWallpaperAudioSystemA18");++checks;
    rejects([&]{RootFacts(123,124,456,true,true,L"MyWallpaperAudioSystemA18");});
    rejects([&]{RootFacts(123,123,456,true,true,L"foreign");});
    rejects([&]{RootFacts(123,123,456,true,false,L"MyWallpaperAudioSystemA18");});
    MatchIdentity(123,123,true,good,good);++checks;
    rejects([&]{MatchIdentity(124,123,true,good,good);});rejects([&]{MatchIdentity(123,123,false,good,good);});rejects([&]{MatchIdentity(123,123,true,"wrong",good);});
    InventoryCoverage complete;complete.Check("synthetic.success",S_OK);Require(complete.Result().at("complete")==true,"successful scoped coverage");++checks;
    InventoryCoverage failed;rejects([&]{failed.Check("synthetic.failedHRESULT",E_ACCESSDENIED);});Require(!failed.complete && !failed.calls.at(0).at("ok").get<bool>(),"failed HRESULT lost");++checks;
    InventoryCoverage invalid;rejects([&]{invalid.Check("synthetic.invalidValue",S_OK,false);});Require(!invalid.complete,"invalid name/id/state not refused");++checks;
    Require(complete.Result().at("excluded").size()==4 && complete.Result().at("scope")=="active-render-endpoints/eMultimedia","coverage scope widened");++checks;
    return {{"purpose","pure-real-reader-contract-tests"},{"checks",checks},{"passed",true},{"COMOrProcessOrWindowOrAudioCalled",false}};
}
static Json ObserveReal(wchar_t** argv) {
    const auto pidValue=Decimal(argv[2]),birth=Decimal(argv[3]),hwndValue=Decimal(argv[5]);
    IdentityBounds(pidValue,hwndValue);
    const DWORD pid=static_cast<DWORD>(pidValue);const auto hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndValue));
    const std::string expected=ImageHash(argv[4]);
    HANDLE raw=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);Require(raw!=nullptr,"Cannot retain process identity");
    std::unique_ptr<void,decltype(&CloseHandle)> process(raw,CloseHandle);
    wchar_t path[32768]{};DWORD pathSize=32768;Require(QueryFullProcessImageNameW(raw,0,path,&pathSize)!=FALSE,"Image path unreadable");
    HANDLE fileRaw=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Require(fileRaw!=INVALID_HANDLE_VALUE,"Cannot retain image bytes");
    std::unique_ptr<void,decltype(&CloseHandle)> image(fileRaw,CloseHandle);
    BY_HANDLE_FILE_INFORMATION initial{};Require(GetFileInformationByHandle(fileRaw,&initial)!=FALSE && GetFileType(fileRaw)==FILE_TYPE_DISK && !(initial.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Image identity unavailable");
    const auto bytesCount=(static_cast<unsigned long long>(initial.nFileSizeHigh)<<32)|initial.nFileSizeLow;
    Require(bytesCount>0 && bytesCount<=32*1024*1024,"Image byte bound");std::string bytes(static_cast<size_t>(bytesCount),'\0');DWORD read=0;
    Require(ReadFile(fileRaw,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE && read==bytes.size(),"Image read incomplete");
    const auto imageHash=service::Sha256(bytes);Require(imageHash==expected,"Image hash differs");
    DWORD rootThread=0;
    auto guard=[&] {
        FILETIME created{},exited{},kernel{},user{};Require(GetProcessTimes(raw,&created,&exited,&kernel,&user)!=FALSE,"Process birth unreadable");MatchIdentity(Number(created),birth,WaitForSingleObject(raw,0)==WAIT_TIMEOUT,imageHash,expected);
        wchar_t currentPath[32768]{};DWORD size=32768;Require(QueryFullProcessImageNameW(raw,0,currentPath,&size)!=FALSE && std::wstring(currentPath,size)==std::wstring(path,pathSize),"Image path changed");
        DWORD owner=0;const DWORD thread=GetWindowThreadProcessId(hwnd,&owner);wchar_t name[128]{};
        Require(IsWindow(hwnd) && GetClassNameW(hwnd,name,128)>0,"Window identity unreadable");
        RootFacts(pid,owner,thread,GetAncestor(hwnd,GA_ROOT)==hwnd,IsWindowVisible(hwnd)!=FALSE,name);
        Require(!rootThread || rootThread==thread,"Window thread changed");rootThread=thread;
    };
    auto message=[](HWND control,UINT kind,WPARAM first,LPARAM second) {
        DWORD_PTR result=0;Require(SendMessageTimeoutW(control,kind,first,second,SMTO_ABORTIFHUNG|SMTO_BLOCK,250,&result)!=0,"Control read timed out/refused");return static_cast<LRESULT>(result);
    };
    auto controls=[&] {
        guard();Json rows=Json::array();
        for(int id:{101,102,103,105,106,116}) {
            HWND control=GetDlgItem(hwnd,id);DWORD owner=0;const auto thread=GetWindowThreadProcessId(control,&owner);wchar_t name[128]{};
            Require(control && GetParent(control)==hwnd && GetDlgCtrlID(control)==id && owner==pid && thread==rootThread && GetClassNameW(control,name,128)>0,"Child identity unreadable/different");
            const wchar_t* wanted=id==101?L"ComboBox":id==116?L"Edit":id==102||id==105?L"msctls_trackbar32":L"Button";
            Require(_wcsicmp(name,wanted)==0,"Child class differs");
            Json row={{"id",id},{"hwnd",std::to_string(reinterpret_cast<uintptr_t>(control))},{"class",Utf8(name)},{"enabled",IsWindowEnabled(control)!=FALSE}};
            if(id==101) {
                const auto count=message(control,CB_GETCOUNT,0,0),index=message(control,CB_GETCURSEL,0,0);Require(count>=0 && count<=4096 && index>=-1 && index<count,"Combo bounds invalid");
                row["count"]=count;row["selection"]=index;
                if(index>=0) {const auto length=message(control,WM_GETTEXTLENGTH,0,0);Require(length>=0 && length<=1024,"Combo text bound");std::vector<wchar_t> text(static_cast<size_t>(length)+1);Require(message(control,WM_GETTEXT,text.size(),reinterpret_cast<LPARAM>(text.data()))==length,"Combo text changed");row["selectedText"]=Utf8(text.data());}else row["selectedText"]=nullptr;
            }
            if(id==116) {const auto length=message(control,WM_GETTEXTLENGTH,0,0);Require(length>=0 && length<=4096,"Status text bound");std::vector<wchar_t> text(static_cast<size_t>(length)+1);Require(message(control,WM_GETTEXT,text.size(),reinterpret_cast<LPARAM>(text.data()))==length,"Status text changed");row["text"]=Utf8(text.data());}
            rows.push_back(std::move(row));
        }
        guard();return rows;
    };
    const auto started=GetTickCount64();
    const auto before=controls();const auto audio=ReadRenderInventory();
    const auto after=controls();Require(before==after,"Controls changed during observation");
    BY_HANDLE_FILE_INFORMATION final{};Require(GetFileInformationByHandle(fileRaw,&final)!=FALSE && initial.dwVolumeSerialNumber==final.dwVolumeSerialNumber && initial.nFileIndexHigh==final.nFileIndexHigh && initial.nFileIndexLow==final.nFileIndexLow && initial.nFileSizeHigh==final.nFileSizeHigh && initial.nFileSizeLow==final.nFileSizeLow && Number(initial.ftLastWriteTime)==Number(final.ftLastWriteTime),"Image file changed");
    guard();
    return {{"purpose","a18-real-instance-readonly-context"},{"pid",pid},{"creationFileTime",std::to_string(birth)},{"imageSha256",expected},{"hwnd",std::to_string(hwndValue)},{"threadId",rootThread},{"startedTick",started},{"finishedTick",GetTickCount64()},{"controls",after},{"independentAudioReadback",audio},{"providerEnvelopeObserved",false},{"inventoryCompletenessVerified",false},{"audioSetterCalled",false},{"inputOrFocusCalled",false},{"childCreated",false}};
}

int wmain(int argc,wchar_t** argv) {
 try {
    if(argc>=2 && std::wstring(argv[1])==L"--observe-real") {Require(argc==6,"--observe-real PID FILETIME sha256:HASH HWND");std::cout<<ObserveReal(argv).dump()<<"\n";return 0;}
    if(argc==2 && std::wstring(argv[1])==L"--observe-real-self-test") {std::cout<<ReaderSelfTest().dump()<<"\n";return 0;}
    Require(argc==2,"expected companion executable");
    { Child child(argv[1]);child.Send({{"v",4},{"type","init"},{"deviceSettings",Json::object()}});Require(child.Read().at("type")=="error","unsupported wire version accepted");child.Exit(1); }
    { Child child(argv[1]);child.Send({{"v",5},{"type","init"},{"deviceSettings",{{"showPanel",true},{"hotkeys",true},{"trayIcon",true},{"ruleEnabled",true},{"soundsEnabled",true},{"deviceSound","C:/fixture-must-not-play.wav"}}}});
      Require(child.Read().at("type")=="ready","ready must be first output");auto state=child.Payload("audio.state");
      Require(state.at("runtime").at("privateFixture")==true,"private runtime identity missing");
      Require(state.at("runtime").at("visible")==false&&state.at("runtime").at("hotkeys")==0&&state.at("runtime").at("tray")==false&&state.at("runtime").at("soundStarted")==false,"fixture escaped isolation");
      auto devices=state.at("endpoints");Require(!devices.empty(),"real endpoint inventory absent");
      child.Send({{"v",5},{"type","message"},{"surface","wallpaper"},{"payload",{{"kind","audio.command"},{"action","endpointVolume"},{"id",devices.at(0).at("id")},{"volume",3},{"requestId","forbidden"}}}});
      auto result=child.Payload("audio.result");Require(result.at("ok")==false&&result.at("requestId")=="forbidden"&&!result.contains("correlationId"),"legacy private master write not rejected");
      child.Payload("audio.state");
      // Exercise the real framed child/Panel::Frame path, not a result builder.
      auto command=[&](const Json& payload){child.Send({{"v",5},{"type","message"},{"surface","wallpaper"},{"payload",payload}});};
      for(const auto& token:{std::string("owner-01234567-89ab-cdef_0.1"),std::string(128,'a')}) {
          command({{"kind","audio.command"},{"action","refresh"},{"requestId","audio-correlated"},{"correlationId",token}});
          const auto success=child.Payload("audio.result");
          Require(success==Json{{"kind","audio.result"},{"requestId","audio-correlated"},{"ok",true},{"correlationId",token}},"correlated audio success changed request or result");child.Payload("audio.state");
      }
      command({{"kind","audio.command"},{"action","endpointVolume"},{"id",devices.at(0).at("id")},{"volume",3},{"requestId","denied-correlated"},{"correlationId","denied-owner"}});
      const auto denied=child.Payload("audio.result");
      Require(denied.at("ok")==false&&denied.at("requestId")=="denied-correlated"&&denied.at("correlationId")=="denied-owner","correlation bypassed private audio permission or was lost on refusal");child.Payload("audio.state");
      command({{"kind","media.command"},{"action","refresh"},{"requestId","media-correlated"},{"correlationId","media-owner"}});
      const auto media=child.Payload("media.result");
      Require(media.at("ok")==false&&media.at("requestId")=="media-correlated"&&media.at("correlationId")=="media-owner","private media refusal lost correlation");child.Payload("audio.state");
      command({{"kind","media.command"},{"action","refresh"},{"requestId","media-legacy"}});
      const auto legacyMedia=child.Payload("media.result");
      Require(legacyMedia.at("ok")==false&&legacyMedia.at("requestId")=="media-legacy"&&!legacyMedia.contains("correlationId"),"legacy media response changed");child.Payload("audio.state");
      for(const auto& invalid:std::vector<Json>{nullptr,42,true,Json::array(),"",std::string(129,'a'),"bad token","bad\nline","non-ascii-é"}) {
          command({{"kind","audio.command"},{"action","endpointVolume"},{"id",devices.at(0).at("id")},{"volume",3},{"requestId","malformed"},{"correlationId",invalid}});
          // The FIFO barrier detects an audio.result for the malformed command;
          // that would mean validation reached dispatch instead of rejecting it.
          command({{"kind","audio.command"},{"action","refresh"},{"requestId","barrier"}});
          bool rejected=false,barrier=false;
          for(int i=0;i<12&&!barrier;++i) {
              const auto frame=child.Read();if(frame.value("type","")!="message")continue;
              const auto& payload=frame.at("payload");const auto kind=payload.value("kind","");
              if(kind=="audio.error") {Require(payload.at("message")=="Identifiant de corrélation invalide."&&!payload.contains("correlationId"),"malformed correlation reflected or wrong rejection");rejected=true;}
              if(kind=="audio.result") {Require(payload.at("requestId")=="barrier"&&payload.at("ok")==true&&!payload.contains("correlationId"),"malformed correlation reached command dispatch or legacy echo changed");barrier=true;}
          }
          Require(rejected&&barrier,"malformed correlation not rejected before live child barrier");
          const auto unchanged=child.Payload("audio.state");
          Require(unchanged.at("endpoints").size()==devices.size(),"endpoint inventory changed during correlation fixture");
          for(const auto& endpoint:devices) {
              const auto& current=unchanged.at("endpoints");auto match=std::find_if(current.begin(),current.end(),[&](const auto& e){return e.at("id")==endpoint.at("id");});
              Require(match!=current.end()&&match->at("volume")==endpoint.at("volume")&&match->at("muted")==endpoint.at("muted"),"endpoint changed during malformed command refusal");
          }
      }
      child.Send({{"v",5},{"type","settings"},{"deviceSettings",{{"panelWidth",10}}}});Require(child.Payload("audio.error").contains("message"),"bad setting accepted");
      child.Send({{"v",5},{"type","message"},{"surface","wallpaper"},{"payload",{{"kind","audio.command"},{"action","refresh"}}}});const auto legacy=child.Payload("audio.result");Require(legacy==Json{{"kind","audio.result"},{"requestId",nullptr},{"ok",true}},"invalid settings corrupted previous config or legacy result changed");
      child.Payload("audio.state");child.Send({{"v",5},{"type","shutdown"}});child.Exit(0);
    }
    { Child child(argv[1]);child.Send({{"v",5},{"type","init"},{"deviceSettings",Json::object()}});Require(child.Read().at("type")=="ready","EOF setup failed");child.Payload("audio.state");CloseHandle(child.input);child.input=nullptr;child.Exit(0); }
    std::cout<<"PASS process-v2 Windows child handshake/version/atomic settings/denied master write/hidden UI/no globals/shutdown/EOF\n";return 0;
 } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
