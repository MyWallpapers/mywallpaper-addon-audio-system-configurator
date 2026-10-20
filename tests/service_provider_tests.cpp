#include "service_provider.hpp"
#include "service_contract_data.hpp"
#include <fstream>
#include <iostream>
#include <thread>

using namespace audio;
using namespace audio::service;
namespace {
void Require(bool value,const char* what) {if(!value)throw std::runtime_error(what);}
template<class F> void Reject(F f) {bool rejected=false;try{f();}catch(...){rejected=true;}Require(rejected,"Expected rejection");}
const Json examples=Json::parse(examplesData);
Json transcript=Json::array();
Json Frame(Json envelope,const std::string& generation="host-generation-1") {
    envelope["type"]="mywallpaper.services";envelope["v"]=1;envelope["generation"]=generation;
    return {{"v",5},{"type","message"},{"surface","service"},{"instanceId",generation},{"displayIndex",0},{"payload",envelope}};
}
int64_t UnixMs() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
struct Fixture {
    std::vector<Json> sent;
    int effects{};
    std::function<void(Operation&)> behavior;
    Provider provider;
    unsigned sequence{};
    std::string host="host-generation-1";
    Fixture():provider([&](const Json& out){
        Require(Json({{"v",5},{"type","message"},{"target","broadcast"},{"payload",out}}).dump().size()<=65536,"Full native frame bound");
        sent.push_back(out);transcript.push_back(out);
    },[&](const Json&,const Json&,Operation& op){op.Check();op.Dispatch();++effects;if(behavior)behavior(op);}) {}
    void Open() {provider.Receive(Frame({{"kind","open"}},host));}
    Json Invoke(const std::string& method,const Json& payload,const std::string& connection="connection-1") {
        return Frame({{"kind","packet"},{"deliveryId","delivery-"+std::to_string(++sequence)},
            {"packet",{{"type","invoke"},{"id","invoke-"+std::to_string(sequence)},{"service","audio"},{"method",method},{"payload",payload},
                {"caller",{{"nodeId","layer:owned"},{"addonId","owned-consumer"},{"generation","caller-generation"}}},
                {"connectionId",connection},{"timeoutMs",30000},{"deadlineUnixMs",UnixMs()+30000}}}},host);
    }
    Json Result(const Json& frame) {
        const auto start=sent.size();provider.Receive(frame);
        Require(sent.size()>=start+2,"Transport ACK and independent result");
        Require(sent[start].at("kind")=="ack","First response is delivery ACK");
        Require(sent[start+1].at("packet").at("type")=="result","Second response is broker result");
        return sent[start+1].at("packet");
    }
    Json Snapshot() {auto result=Result(Invoke("snapshot",examples.at("snapshotRequest")));Require(result.at("ok")==true,"Snapshot call succeeds");return result.at("value");}
    Json Command(const std::string& id="command-1") {
        auto state=Snapshot().at("audio");auto request=examples.at("commandRequest");
        request["generation"]=host;request["revision"]=state.at("revision");request["requestId"]=id;return request;
    }
    void Audio() {
        auto raw=examples.at("audioState").at("body").at("value");raw["runtime"]["hwnd"]=uint64_t(123456789);
        provider.AudioState(raw,true,false);
    }
    void Media() {
        const auto& value=examples.at("mediaState").at("body").at("value");auto raw=value.at("selected");
        raw["sessionId"]=raw.at("id");raw["status"]="ready";raw["sessions"]=value.at("sessions");provider.MediaState(raw,true,true);
    }
};
void TransportAndProjection() {
    Provider failed([](const Json&){throw std::runtime_error("owned send failure");});
    Reject([&]{failed.Receive(Frame({{"kind","open"}}));});Require(!failed.IsOpen(),"Send failure revokes the provider");
    Fixture f;auto spoof=Frame({{"kind","open"}});spoof["surface"]="wallpaper";
    Require(f.provider.Receive(spoof)&&!f.provider.IsOpen()&&f.sent.empty(),"Surface lookalike cannot open a provider");
    f.Open();Require(f.sent.size()==3,"Provide and both initial states");
    Require(f.sent[0].at("packet").at("type")=="provide","Canonical provide declaration");
    auto initial=f.Snapshot();Require(initial.at("audio").at("availability")=="absent","Initial absent snapshot");
    f.Audio();f.Media();auto state=f.Snapshot();
    Validate(state,"snapshotResult");
    auto audio=state.at("audio").at("body").at("value");
    Require(audio.at("runtime").at("hwnd")=="123456789","Exact decimal HWND projection");
    Require(audio.at("endpoints")[0].at("capabilities").at("volume")==true,"Render capability observed");
    Require(audio.at("endpoints")[1].at("capabilities").at("volume")==false,"Mic permission retained");
    auto media=state.at("media").at("body").at("value");
    Require(media.at("sessions")[1].at("capabilities").is_null(),"No invented capability for unobserved player");
    const auto sent=f.sent.size();f.Audio();Require(f.sent.size()==sent,"Unchanged observation emits no false revision");
    f.provider.AudioState(Json::object(),false,false);Require(f.Snapshot().at("audio").at("availability")=="absent","Disable invalidates prior audio state");
    auto raw=examples.at("audioState").at("body").at("value");raw["runtime"]["hwnd"]=0;raw["sessions"][0]["endpointId"]="foreign";
    f.provider.AudioState(raw,true,false);Require(f.Snapshot().at("audio").at("availability")=="unavailable","Orphaned session is not silently dropped");
    f.provider.MediaState({{"status","unavailable"},{"message","owned failure"}},true,true);
    Require(f.Snapshot().at("media").at("availability")=="unavailable","Failed media distinct from empty");
}
void Commands() {
    Fixture f;f.Open();f.Audio();auto request=f.Command();
    auto result=f.Result(f.Invoke("command",request)).at("value");Validate(result,"commandResult");
    Require(result.at("ok")==true&&result.at("requestId")==request.at("requestId")&&f.effects==1,"Correlated execution");
    auto repeated=f.Result(f.Invoke("command",request)).at("value");Require(repeated.at("error").at("code")=="invalid_request"&&f.effects==1,"Duplicate never redispatched");
    Require(f.Result(f.Invoke("command",request,"connection-2")).at("value").at("ok")==true&&f.effects==2,"Same ID on distinct host connection is independent");
    request=f.Command("missing-context");auto missing=f.Invoke("command",request);missing["payload"]["packet"].erase("connectionId");
    Require(f.Result(missing).at("value").at("outcome")=="rejected"&&f.effects==2,"Absent trusted context fails before effect");
    request=f.Command("stale-generation");request["generation"]="old-generation";
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="stale_generation","Business generation guard");
    request=f.Command("stale-revision");request["revision"]=0;
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="stale_revision","Topic revision guard");
    request=f.Command("capture");request["command"]={{"kind","audio.command"},{"action","endpointMute"},{"id","capture-owned"},{"muted",true}};
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="permission_denied"&&f.effects==2,"No capture write without permission");
    f.Media();auto media=f.Snapshot().at("media");request=f.Command("pinned");
    request["revision"]=media.at("revision");request["expectedSessionId"]="missing";request["command"]={{"kind","media.command"},{"action","playPause"}};
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="stale_session","No fallback to another player");
    request["requestId"]="no-capability";request["expectedSessionId"]="media-owned";request["command"]["action"]="next";
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="unsupported","Per-session capability guard");
    request=f.Command("forged");request["caller"]={{"nodeId","administrator"}};
    Require(f.Result(f.Invoke("command",request)).at("ok")==false&&f.effects==2,"Caller identity cannot come from business payload");
}
void CancellationAndDeadlines() {
    Fixture f;f.Open();f.Audio();auto request=f.Command("expired-host");auto expired=f.Invoke("command",request);
    expired["payload"]["packet"]["deadlineUnixMs"]=UnixMs()-1000;
    Require(f.Result(expired).at("value").at("error").at("code")=="timeout"&&f.effects==0,"Delayed host packet cannot dispatch");
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="invalid_request"&&f.effects==0,"Expired command ID cannot be reused");
    request=f.Command("queue-expiry");auto queued=f.Invoke("command",request);queued["payload"]["packet"]["timeoutMs"]=1;
    f.provider.Prepare(queued);std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(f.Result(queued).at("value").at("outcome")=="rejected"&&f.effects==0,"Queue delay consumes monotone budget");
    request=f.Command("payload-queue-expiry");request["timeoutMs"]=1;queued=f.Invoke("command",request);
    f.provider.Prepare(queued);std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(f.Result(queued).at("value").at("outcome")=="rejected"&&f.effects==0,"Business timeout includes queue delay");
    request=f.Command("invalid-host-expiry");queued=f.Invoke("command",request);queued["payload"]["packet"]["deadlineUnixMs"]=INT64_MIN;
    Require(f.Result(queued).at("value").at("outcome")=="rejected"&&f.effects==0,"Invalid host expiry fails closed without overflow");
    request=f.Command("cancel-before");auto before=f.Invoke("command",request);
    auto cancel=Frame({{"kind","packet"},{"deliveryId","cancel-delivery"},{"packet",{{"type","invoke.cancel"},{"id",before.at("payload").at("packet").at("id")}}}});
    f.provider.Prepare(before);f.provider.Prepare(cancel);
    Require(f.Result(before).at("value").at("error").at("code")=="cancelled"&&f.effects==0,"Cancel before UI dispatch");
    request=f.Command("cancel-after");auto after=f.Invoke("command",request);
    cancel["payload"]["packet"]["id"]=after.at("payload").at("packet").at("id");
    f.behavior=[&](Operation& op){f.provider.Prepare(cancel);op.Check();};
    auto unknown=f.Result(after).at("value");
    Require(unknown.at("outcome")=="unknown"&&unknown.at("error").at("code")=="cancelled"&&f.effects==1,"Cancellation after dispatch has uncertain outcome");
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="invalid_request"&&f.effects==1,"Uncertain command not replayed");
    f.behavior=[&](Operation& op){f.provider.Revoke();op.Check();};request=f.Command("eof-after");
    Require(f.Result(f.Invoke("command",request)).at("value").at("error").at("code")=="revoked","EOF revokes in-flight operation");
    f.behavior=[](Operation&){throw std::runtime_error("native failure after effect");};request=f.Command("native-error");
    Require(f.Result(f.Invoke("command",request)).at("value").at("outcome")=="unknown","Untyped native failure is not proof of no effect");
}
void Inventories() {
    Fixture f;f.Open();auto raw=examples.at("audioState").at("body").at("value");raw["runtime"]["hwnd"]=0;
    auto item=raw.at("sessions")[0];raw["sessions"]=Json::array();
    for(int i=0;i<100;++i) {auto current=item;current["id"]="owned-"+std::to_string(i);current["name"]=std::string(1000,'x')+"é🎵";raw["sessions"].push_back(current);}
    f.provider.AudioState(raw,true,false);auto state=f.Snapshot().at("audio");
    Require(state.at("body").at("mode")=="paged","Large inventory is paged, not truncated");
    Json request={{"kind","audio-system.inventory.request"},{"requestId","inventory-read"},{"generation",f.host},{"topic","audio"},
        {"revision",state.at("revision")},{"inventoryId",state.at("body").at("inventoryId")},{"offset",0}};
    size_t pages=0;bool last=false;
    while(!last) {
        auto result=f.Result(f.Invoke("inventory",request)).at("value");Validate(result,"inventoryResult");
        Require(result.at("sha256")==state.at("body").at("sha256"),"Immutable checksum repeated");
        ++pages;last=result.at("nextOffset").is_null();if(!last)request["offset"]=result.at("nextOffset");
        Require(pages<=512,"Inventory page count bound");
    }
    Require(pages>1,"Continuation actually exercised");
    auto obsolete=request;raw["runtime"]["notice"]="changed revision";f.provider.AudioState(raw,true,false);
    Require(f.Result(f.Invoke("inventory",obsolete)).at("value").at("error").at("code")=="inventory_expired","Replacement invalidates old continuation");
    raw["runtime"]["notice"]=std::string(16*1024*1024+1,'x');f.provider.AudioState(raw,true,false);
    auto refused=f.Snapshot().at("audio");Require(refused.at("availability")=="unavailable"&&refused.at("error").at("code")=="message_too_large","Oversize refused observably");
    f.provider.Receive(Frame({{"kind","closed"},{"reason","owned close"}}));Require(!f.provider.IsOpen(),"Generation closed");
    f.host="host-generation-2";f.Open();Require(f.Snapshot().at("audio").at("availability")=="absent","Reopen starts with no retained old state");
    auto late=Frame({{"kind","packet"},{"deliveryId","old-delivery"},{"packet",{{"type","invoke.cancel"},{"id","old"}}}});
    f.provider.Receive(late);Require(!f.provider.IsOpen(),"Revoked generation cannot acquire new route");
}
}
int main(int argc,char** argv) {
    try {
        Require(Sha256("")=="sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty known answer");
        Require(Sha256("abc")=="sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 short known answer");
        Require(Sha256(std::string(1000000,'a'))=="sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA256 multi-block known answer");
        Reject([]{Validate({{"kind","audio.command"},{"action","setDefaultDevice"}},"audioCommand");});
        TransportAndProjection();Commands();CancellationAndDeadlines();Inventories();
        if(argc==2){std::ofstream out(argv[1]);out<<transcript.dump();if(!out)throw std::runtime_error("Transcript output failed");}
        std::cout<<"PASS actual native provider: transport, projections, commands, deadlines/cancel/revoke, no replay, inventories, SHA-256\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
