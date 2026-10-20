#include "audio.hpp"
#include <audioclient.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <set>
#include <stdexcept>
using namespace audio;
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
struct Stream {
    winrt::com_ptr<IAudioClient> client;
    WAVEFORMATEX* format{};
    ~Stream(){if(client)client->Stop();CoTaskMemFree(format);}
};

// This opt-in mode owns exactly one NOPERSIST silent session. It never accepts
// a target ID or a setter request: only the real add-on may perform the stimulus.
static DWORD HoldBudget(const std::string& text) {
    Require(!text.empty() && text.size()<=6 && text.find_first_not_of("0123456789")==std::string::npos,"Invalid hold budget");
    const auto value=std::stoul(text);Require(value>=1000 && value<=180000,"Hold budget outside 1000..180000ms");return DWORD(value);
}
static Json HeldRequest(const std::string& line) {
    Require(!line.empty() && line.size()<=2048,"Bounded request required");
    auto j=Json::parse(line);Require(j.is_object() && j.size()>=2 && j.contains("requestId") && j["requestId"].is_string(),"Request shape");
    const auto id=j.at("requestId").get<std::string>();Require(!id.empty() && id.size()<=64 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==std::string::npos,"Request ID");
    Require(j.contains("action") && j["action"].is_string(),"Request action");
    const auto action=j.at("action").get<std::string>();Require(action=="observe"||action=="restore"||action=="stop","Unsupported action");
    Require(j.size()==(action=="restore"?3:2),"Unknown request fields");
    if(action=="restore") {
        const auto& e=j.at("expected");Require(e.is_object() && e.size()==2 && e.contains("volume") && e["volume"].is_number() && e.contains("muted") && e["muted"].is_boolean(),"Expected postimage required");
        double v=e.at("volume");Require(std::isfinite(v) && v>=0 && v<=1,"Invalid expected scalar");
    }
    return j;
}
static bool SameLevel(const Json& a,const Json& b) {
    return a.at("volume").get<float>()==b.at("volume").get<float>() && a.at("muted")==b.at("muted");
}
template<class Read,class Write> static Json RestoreOwnedLevel(Read read,Write write,const Json& baseline,const Json& expected) {
    const auto current=read();Require(SameLevel(current.at("session"),expected),"Owned session postimage changed; restoration withheld");
    write(baseline.at("session"));
    const auto restored=read();Require(SameLevel(restored.at("session"),baseline.at("session")),"Owned restoration readback differs");return restored;
}
template<class Read> static Json StoppedOwnedLevel(Read read,const Json& baseline) {
    const auto current=read();Require(SameLevel(current.at("session"),baseline.at("session")),"Owned session changed after restore");return current;
}
static void HeldPureTest() {
    unsigned checks=0;
    auto good=[&](bool value){Require(value,"Held-session pure assertion");++checks;};
    auto refused=[&](auto f){bool failed=false;try{f();}catch(...){failed=true;}good(failed);};
    good(HoldBudget("1000")==1000);good(HoldBudget("180000")==180000);
    for(auto s:{"","999","180001","-1000","1000x","9999999"})refused([&]{HoldBudget(s);});
    good(HeldRequest(R"({"action":"observe","requestId":"read-1"})").at("action")=="observe");
    good(HeldRequest(R"({"action":"stop","requestId":"stop_1"})").at("action")=="stop");
    good(HeldRequest(R"({"action":"restore","requestId":"r","expected":{"volume":0.5,"muted":false}})").size()==3);
    for(auto s:{R"({"action":"sessionVolume","requestId":"r"})",R"({"action":"observe","requestId":"r","id":"foreign"})",R"({"action":"restore","requestId":"r"})",R"({"action":"restore","requestId":"r","expected":{"volume":2,"muted":false}})",R"({"action":"observe","requestId":"bad id"})",R"({"action":"observe","requestId":"r","expected":{}})"})refused([&]{HeldRequest(s);});
    refused([&]{HeldRequest(std::string(2049,'x'));});
    good(SameLevel({{"volume",.5f},{"muted",false}},{{"volume",.5f},{"muted",false}}));
    good(!SameLevel({{"volume",.5f},{"muted",false}},{{"volume",.6f},{"muted",false}}));
    good(!SameLevel({{"volume",.5f},{"muted",false}},{{"volume",.5f},{"muted",true}}));
    const Json baseline={{"session",{{"volume",.75f},{"muted",false}}}};
    Json current={{"session",{{"volume",.25f},{"muted",true}}}};int reads=0,writes=0;
    auto read=[&]{++reads;return current;};auto write=[&](const Json& value){++writes;current["session"]=value;};
    auto expected=current.at("session");auto result=RestoreOwnedLevel(read,write,baseline,expected);
    good(reads==2 && writes==1 && result==baseline);
    for(const auto& changed:{Json{{"volume",.5f},{"muted",true}},Json{{"volume",.25f},{"muted",false}}}) {
        current["session"]=changed;writes=0;refused([&]{RestoreOwnedLevel(read,write,baseline,expected);});good(writes==0);
    }
    writes=0;refused([&]{RestoreOwnedLevel([]()->Json{throw std::runtime_error("COM unavailable");},write,baseline,expected);});good(writes==0);
    current["session"]=expected;refused([&]{RestoreOwnedLevel(read,[](const Json&){throw std::runtime_error("setter failed");},baseline,expected);});
    refused([&]{RestoreOwnedLevel(read,[](const Json&){},baseline,expected);});
    for(const auto& external:{Json{{"volume",.5f},{"muted",false}},Json{{"volume",.75f},{"muted",true}}}) {
        current["session"]=expected;
        auto last=RestoreOwnedLevel(read,write,baseline,expected); // Successful explicit restore.
        current["session"]=external;writes=0;int exitCode=0;bool fallbackRefused=false;
        try {last=StoppedOwnedLevel(read,baseline);}
        catch(const std::exception& error) {
            good(std::string(error.what())=="Owned session changed after restore");exitCode=1;
            try {last=RestoreOwnedLevel(read,write,baseline,last.at("session"));}
            catch(...) {fallbackRefused=true;}
        }
        good(exitCode!=0 && fallbackRefused && writes==0);
        good(last==baseline && current.at("session")==external);
    }
    current=baseline;good(StoppedOwnedLevel(read,baseline)==baseline);
    std::cout<<Json{{"state","passed"},{"pureChecks",checks},{"audioSessionOpened",false}}.dump()<<"\n";
}
static int HoldOwnedSession(DWORD budget,Session& selected,Endpoint& endpoint) {
    HANDLE input=GetStdHandle(STD_INPUT_HANDLE);Require(input && input!=INVALID_HANDLE_VALUE && GetFileType(input)==FILE_TYPE_PIPE,"Private stdin pipe required");
    const auto started=GetTickCount64(),deadline=started+budget;
    FILETIME born{},exit{},kernel{},user{};Require(GetProcessTimes(GetCurrentProcess(),&born,&exit,&kernel,&user)!=FALSE,"Process FILETIME unavailable");
    const auto birth=(uint64_t(born.dwHighDateTime)<<32)|born.dwLowDateTime;
    const auto own=selected.id,target=endpoint.id;
    auto take=[](LPWSTR value){Require(value!=nullptr,"COM identity absent");const auto text=Utf8(value);CoTaskMemFree(value);Require(!text.empty(),"COM identity empty");return text;};
    auto sample=[&] {
        DWORD pid{};Require(selected.control->GetProcessId(&pid)==S_OK && pid==GetCurrentProcessId(),"Session is not single-process owned");
        LPWSTR instance{},device{};winrt::check_hresult(selected.control->GetSessionInstanceIdentifier(&instance));Require(take(instance)==own,"Session identity changed");
        winrt::check_hresult(endpoint.device->GetId(&device));Require(take(device)==target,"Endpoint identity changed");
        EDataFlow flow{};winrt::check_hresult(endpoint.device.as<IMMEndpoint>()->GetDataFlow(&flow));Require(flow==eRender,"Render endpoint required");
        AudioSessionState state{};winrt::check_hresult(selected.control->GetState(&state));Require(state==AudioSessionStateActive,"Owned session not active");
        float volume{},master{};BOOL muted{},masterMuted{};
        winrt::check_hresult(selected.mixer->GetMasterVolume(&volume));winrt::check_hresult(selected.mixer->GetMute(&muted));
        winrt::check_hresult(endpoint.control->GetMasterVolumeLevelScalar(&master));winrt::check_hresult(endpoint.control->GetMute(&masterMuted));
        Require(std::isfinite(volume)&&volume>=0&&volume<=1&&std::isfinite(master)&&master>=0&&master<=1,"Invalid COM scalar");
        return Json{{"session",{{"volume",volume},{"muted",muted!=FALSE}}},{"endpoint",{{"volume",master},{"muted",masterMuted!=FALSE}}},{"observedTick",GetTickCount64()}};
    };
    const auto baseline=sample();auto last=baseline;bool restored=false;
    auto emit=[&](const std::string& state,const std::string& id,const Json& observation){std::cout<<Json{{"protocol",1},{"state",state},{"requestId",id},{"identity",{{"pid",GetCurrentProcessId()},{"creationFileTime",std::to_string(birth)},{"sessionId",own},{"endpointId",target}}},{"preimage",baseline},{"observation",observation},{"deadlineTick",deadline},{"coverage","owned-session-and-selected-render-endpoint-only"}}.dump()<<std::endl;Require(bool(std::cout),"Private stdout unavailable");};
    auto restore=[&](const Json& expected){
        last=RestoreOwnedLevel(sample,[&](const Json& original) {
            // The only held-mode setters use this fixture's retained COM pointer,
            // never a supplied session/endpoint ID. Guard precedes both effects.
            winrt::check_hresult(selected.mixer->SetMasterVolume(original.at("volume").get<float>(),nullptr));
            winrt::check_hresult(selected.mixer->SetMute(original.at("muted").get<bool>(),nullptr));
        },baseline,expected);restored=true;
    };
    try {
        emit("ready","",baseline);std::string pending;std::set<std::string> requests;
        while(GetTickCount64()<deadline) {
            DWORD available{};
            if(!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr)){Require(GetLastError()==ERROR_BROKEN_PIPE,"Private pipe read failed");throw std::runtime_error("Private input closed before stop");}
            if(!available){Sleep(10);continue;}
            char c{};DWORD count{};Require(ReadFile(input,&c,1,&count,nullptr)!=FALSE&&count==1,"Private pipe read failed");
            if(c!='\n'){pending+=c;Require(pending.size()<=2048,"Request exceeds bound");continue;}
            const auto request=HeldRequest(pending);pending.clear();const auto id=request.at("requestId").get<std::string>();Require(requests.size()<32&&requests.insert(id).second,"Request count or duplicate ID");
            const auto action=request.at("action").get<std::string>();
            if(action=="observe"){last=sample();restored=false;emit("observation",id,last);}
            else if(action=="restore"){restore(request.at("expected"));emit("restored",id,last);}
            else {Require(restored,"Explicit restore required before normal stop");last=StoppedOwnedLevel(sample,baseline);emit("stopped",id,last);return 0;}
        }
        throw std::runtime_error("Held-session deadline expired");
    } catch(...) {
        // Only a known last postimage permits fallback restoration. A mutation
        // not yet observed is unknown, never overwritten to make cleanup pass.
        try {restore(last.at("session"));emit("restored-after-refusal","",last);} catch(...) {try{emit("restoration-unverified","",Json::object());}catch(...) {}}
        throw;
    }
}

int main(int argc,char** argv) {
 try {
    if(argc==2 && std::string(argv[1])=="--held-session-self-test"){HeldPureTest();return 0;}
    const bool held=argc==3 && std::string(argv[1])=="--hold-owned-session";
    Require(argc==1||held,"Expected no args, --held-session-self-test or --hold-owned-session TIMEOUT_MS");
    const auto budget=held?HoldBudget(argv[2]):0;
    if(held){auto input=GetStdHandle(STD_INPUT_HANDLE);Require(input && input!=INVALID_HANDLE_VALUE && GetFileType(input)==FILE_TYPE_PIPE,"Private stdin pipe required");}
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Audio backend(GetCurrentProcessId());backend.Refresh();
    std::map<std::string,std::pair<float,bool>> before;
    std::string target;
    for(const auto& device:backend.endpoints){before[device.id]={device.volume,device.muted};if(!device.capture&&device.isDefault)target=device.id;}
    Require(!target.empty(),"A real render endpoint is required; no fixture success without it.");
    auto device=backend.Device(target).device;
    Stream stream;
    winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,stream.client.put_void()));
    winrt::check_hresult(stream.client->GetMixFormat(&stream.format));
    GUID session{};winrt::check_hresult(CoCreateGuid(&session));
    winrt::check_hresult(stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_NOPERSIST,1000000,0,stream.format,&session));
    UINT32 size{};winrt::check_hresult(stream.client->GetBufferSize(&size));
    winrt::com_ptr<IAudioRenderClient> renderer;winrt::check_hresult(stream.client->GetService(__uuidof(IAudioRenderClient),renderer.put_void()));
    BYTE* buffer{};winrt::check_hresult(renderer->GetBuffer(size,&buffer));
    winrt::check_hresult(renderer->ReleaseBuffer(size,AUDCLNT_BUFFERFLAGS_SILENT));
    winrt::com_ptr<IAudioSessionControl> ownControl;
    winrt::check_hresult(stream.client->GetService(__uuidof(IAudioSessionControl),ownControl.put_void()));
    winrt::check_hresult(ownControl->SetDisplayName(L"A18 owned silent fixture",nullptr));
    winrt::check_hresult(stream.client->Start());
    backend.Refresh();std::string own;
    for(const auto& value:backend.sessions)if(value.pid==GetCurrentProcessId()&&value.name=="A18 owned silent fixture")own=value.id;
    Require(!own.empty(),"The owned WASAPI session was not enumerated.");
    auto& selected=backend.App(own);float original=selected.volume;bool muted=selected.muted;
    if(held){
        LPWSTR createdId{};winrt::check_hresult(ownControl.as<IAudioSessionControl2>()->GetSessionInstanceIdentifier(&createdId));
        const auto createdSession=createdId?Utf8(createdId):std::string{};CoTaskMemFree(createdId);
        Require(!createdSession.empty() && createdSession==own,"Enumerated session differs from the newly created stream");
        return HoldOwnedSession(budget,selected,backend.Device(target));
    }
    struct Restore {winrt::com_ptr<ISimpleAudioVolume> volume;float original;bool muted;~Restore(){volume->SetMasterVolume(original,nullptr);volume->SetMute(muted,nullptr);}} restore{selected.mixer,original,muted};
    backend.SetSession(own,.37f,true);backend.Refresh();
    Require(std::abs(backend.App(own).volume-.37f)<.002f&&backend.App(own).muted,"Owned session write was not observed.");
    backend.SetSession(own,original,muted);backend.Refresh();
    Require(std::abs(backend.App(own).volume-original)<.002f&&backend.App(own).muted==muted,"Owned session restoration failed.");
    bool denied=false;try{backend.SetEndpoint(target,.1f,{},true);}catch(...){denied=true;}Require(denied,"Private fixture allowed a master volume write.");
    for(const auto& endpoint:backend.endpoints) {
        if(!before.contains(endpoint.id))continue;
        Require(std::abs(endpoint.volume-before.at(endpoint.id).first)<.002f&&endpoint.muted==before.at(endpoint.id).second,"An endpoint changed during the fixture (including possible concurrent external change).");
    }
    auto snapshot=backend.Snapshot();Require(snapshot.at("sessions").size()>0,"Empty fixture snapshot.");
    std::cout<<"PASS real Windows WASAPI owned silent stream; own volume/mute/restore; endpoint writes rejected; all endpoint volumes/mutes unchanged. PID="<<GetCurrentProcessId()<<"\n";
    return 0;
 } catch(const winrt::hresult_error& error) {std::cerr<<winrt::to_string(error.message())<<'\n';return 1;}
   catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
