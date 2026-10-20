#include "wire.hpp"
#include "service_provider.hpp"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace audio;
namespace {
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Child {
    HANDLE process{},input{},output{};DWORD pid{};
    explicit Child(const wchar_t* executable) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE readIn{},writeOut{};
        Require(CreatePipe(&readIn,&input,&security,0)&&CreatePipe(&output,&writeOut,&security,0),"Owned pipes");
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESTDHANDLES;start.hStdInput=readIn;start.hStdOutput=writeOut;start.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION info{};std::wstring command=L"\""+std::wstring(executable)+L"\" --private-fixture";
        const auto ok=CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&info);
        CloseHandle(readIn);CloseHandle(writeOut);Require(ok!=FALSE,"Owned hidden companion launch");
        process=info.hProcess;pid=info.dwProcessId;CloseHandle(info.hThread);
    }
    ~Child(){if(process){if(WaitForSingleObject(process,0)==WAIT_TIMEOUT)TerminateProcess(process,99);CloseHandle(process);}if(input)CloseHandle(input);if(output)CloseHandle(output);}
    void Send(const Json& value){std::ostringstream stream;wire::Write(stream,value);auto bytes=stream.str();DWORD written{};Require(WriteFile(input,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size(),"Owned pipe write");}
    void ReadExact(char* bytes,DWORD count) {
        DWORD offset{};const auto deadline=GetTickCount64()+5000;
        while(offset<count) {
            DWORD available{};Require(PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)!=FALSE,"Owned pipe remains open");
            if(!available){Require(GetTickCount64()<deadline,"Owned response deadline");Sleep(5);continue;}
            DWORD read{};Require(ReadFile(output,bytes+offset,std::min(count-offset,available),&read,nullptr)&&read,"Owned pipe read");offset+=read;
        }
    }
    Json Read() {
        unsigned char header[4]{};ReadExact(reinterpret_cast<char*>(header),4);
        const auto size=uint32_t(header[0])|uint32_t(header[1])<<8|uint32_t(header[2])<<16|uint32_t(header[3])<<24;
        Require(size>0&&size<=65536,"Actual full frame <=64KiB");std::string bytes(size,'\0');ReadExact(bytes.data(),size);return Json::parse(bytes);
    }
    void Exit(){Require(WaitForSingleObject(process,5000)==WAIT_OBJECT_0,"Owned companion normal stop");DWORD code{};GetExitCodeProcess(process,&code);Require(code==0,"Owned companion exit code");}
};
const std::string generation="owned-provider-generation";
Json Frame(Json envelope) {
    envelope.update({{"type","mywallpaper.services"},{"v",1},{"generation",generation}});
    return {{"v",5},{"type","message"},{"surface","service"},{"instanceId",generation},{"displayIndex",0},{"payload",envelope}};
}
void Isolated(const Json& runtime,DWORD expectedPid) {
    Require(runtime.at("privateFixture")==true&&runtime.at("pid")==expectedPid,"Exact owned process identity");
    Require(runtime.at("visible")==false&&runtime.at("tray")==false&&runtime.at("hotkeys")==0&&runtime.at("soundStarted")==false,"No visible/global/audio resources");
    const auto hwnd=runtime.at("hwnd").is_string()?std::stoull(runtime.at("hwnd").get<std::string>()):runtime.at("hwnd").get<uint64_t>();
    auto window=reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));DWORD owner{};GetWindowThreadProcessId(window,&owner);
    Require(IsWindow(window)&&!IsWindowVisible(window)&&owner==expectedPid,"Hidden owned dispatch window");
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        Require(argc==3,"Companion and transcript paths required");Child child(argv[1]);Json transcript=Json::array();
        child.Send({{"v",5},{"type","init"},{"deviceSettings",{{"showPanel",true},{"trayIcon",true},{"hotkeys",true},{"ruleEnabled",true},{"soundsEnabled",true},{"deviceSound","C:/fixture-must-not-play.wav"}}}});
        Require(child.Read().at("type")=="ready","Process ready first");
        bool initial=false;
        for(int i=0;i<16&&!initial;++i){auto frame=child.Read();if(frame.value("type","")=="message"&&frame.at("payload").value("kind","")=="audio.state"){Isolated(frame.at("payload").at("runtime"),child.pid);initial=true;}}
        Require(initial,"Owned runtime observation");child.Send(Frame({{"kind","open"}}));
        auto serviceFrame=[&] {
            for(int i=0;i<32;++i){auto frame=child.Read();if(frame.value("type","")!="message")continue;auto payload=frame.at("payload");
                if(payload.value("type","")=="mywallpaper.services"){transcript.push_back(payload);return payload;}}
            throw std::runtime_error("Expected native service frame");
        };
        Require(serviceFrame().at("packet").at("type")=="provide","Provide precedes publish as required by generic broker");
        for(const auto* topic:{"audio","media"}) {
            auto packet=serviceFrame().at("packet");Require(packet.at("type")=="publish"&&packet.at("topic")==topic,"Both initial publications ordered");
            service::Validate(packet.at("value"),std::string(topic)+"State");Require(packet.at("value").at("availability")=="absent","Initial absent state explicit");
        }
        const auto expiry=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()+5000;
        auto invoke=[&](const std::string& id,const std::string& method,Json payload){child.Send(Frame({{"kind","packet"},{"deliveryId","delivery-"+id},{"packet",{{"type","invoke"},{"id",id},{"service","audio"},{"method",method},{"payload",payload},{"caller",{{"nodeId","owned-layer"},{"addonId","owned-consumer"},{"generation","owned-caller"}}},{"connectionId","owned-connection"},{"timeoutMs",5000},{"deadlineUnixMs",expiry}}}}));};
        auto result=[&](const std::string& id){bool ack=false;for(int i=0;i<32;++i){auto envelope=serviceFrame();if(envelope.at("kind")=="ack"&&envelope.at("deliveryId")=="delivery-"+id)ack=true;
            if(envelope.at("kind")=="packet"&&envelope.at("packet").at("type")=="result"&&envelope.at("packet").at("id")==id){Require(ack,"Delivery ACK separate from result");return envelope.at("packet");}}throw std::runtime_error("Expected correlated result");};
        invoke("snapshot","snapshot",{{"kind","audio-system.snapshot.request"},{"requestId","owned-snapshot"}});
        auto snapshot=result("snapshot");Require(snapshot.at("ok")==true,"Real Panel snapshot RPC");snapshot=snapshot.at("value");service::Validate(snapshot,"snapshotResult");
        const auto& audio=snapshot.at("audio");Require(audio.at("availability")=="ready"||audio.at("availability")=="empty","Actual read-only audio projection usable");
        Require(audio.at("body").at("mode")=="inline","Owned ordinary inventory inline");const auto& value=audio.at("body").at("value");Isolated(value.at("runtime"),child.pid);
        for(const auto* group:{"endpoints","sessions"})for(const auto& item:value.at(group))Require(item.at("capabilities").at("volume")==false&&item.at("capabilities").at("mute")==false,"Private fixture never advertises write capabilities");
        Require(snapshot.at("media").at("availability")=="absent","Private fixture never collects external media");
        invoke("command","command",{{"kind","audio-system.command.request"},{"requestId","owned-refusal"},{"generation",generation},{"revision",audio.at("revision")},{"timeoutMs",5000},{"expectedSessionId",nullptr},{"command",{{"kind","audio.command"},{"action","show"}}}});
        auto refused=result("command");Require(refused.at("ok")==true,"Structured business refusal");service::Validate(refused.at("value"),"commandResult");
        Require(refused.at("value").at("outcome")=="rejected"&&refused.at("value").at("error").at("code")=="unsupported","No executor or visible command in private fixture");
        child.Send(Frame({{"kind","closed"},{"reason","Owned fixture complete"}}));child.Send({{"v",5},{"type","shutdown"}});child.Exit();
        std::ofstream output(argv[2],std::ios::binary);output<<transcript.dump();Require(bool(output),"Transcript written");
        std::cout<<"PASS hidden actual companion: owned window, zero global resources, non-writable caps, provide/publish, snapshot/command refusal, normal stop\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
