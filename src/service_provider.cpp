#include "service_provider.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace audio::service {
namespace {
constexpr size_t jsonLimit=65536, inventoryLimit=16*1024*1024, chunkSize=32768;
Json Error(const std::string& code,const std::string& message) {
    // Diagnostics alone are bounded; state/metadata is never truncated.
    size_t end=0,characters=0;
    while(end<message.size()) {if((static_cast<unsigned char>(message[end])&0xc0)!=0x80 && ++characters>512)break;++end;}
    return {{"code",code},{"message",message.substr(0,end)}};
}
bool Token(const Json& value) {
    try {Validate(value,"token");return true;}catch(...){return false;}
}
bool FrameFits(const Json& envelope) {
    return Json({{"v",5},{"type","message"},{"target","broadcast"},{"payload",envelope}}).dump().size()<=jsonLimit;
}
std::string Base64(const std::string& bytes) {
    constexpr auto alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve(((bytes.size()+2)/3)*4);
    for(size_t i=0;i<bytes.size();i+=3) {
        auto a=static_cast<unsigned char>(bytes[i]);auto b=i+1<bytes.size()?static_cast<unsigned char>(bytes[i+1]):0;
        auto c=i+2<bytes.size()?static_cast<unsigned char>(bytes[i+2]):0;
        out+=alphabet[a>>2];out+=alphabet[((a&3)<<4)|(b>>4)];
        out+=i+1<bytes.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';out+=i+2<bytes.size()?alphabet[c&63]:'=';
    }
    return out;
}
void Unique(const Json& items) {
    std::set<std::string> seen;
    for(const auto& item:items)if(!seen.insert(item.at("id").get<std::string>()).second)throw std::runtime_error("Duplicate provider-local ID");
}
Json NoMediaControls() {return {{"playPause",false},{"previous",false},{"next",false},{"seek",false},{"shuffle",false},{"repeat",false}};}
}

void Operation::Check() const {
    const auto reason=cancelled.load();
    if(reason)throw Failure(reason==2?"revoked":"cancelled","Service operation cancelled or revoked");
    const auto unixNow=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if(std::chrono::steady_clock::now()>=deadline || (deadlineUnixMs&&unixNow>=deadlineUnixMs))throw Failure("timeout","Service operation deadline exceeded");
}
Provider::Provider(std::function<void(const Json&)> send,std::function<void(const Json&,const Json&,Operation&)> execute):send_(std::move(send)),execute_(std::move(execute)) {}
void Provider::Prepare(const Json& frame) {
    try {
        if(frame.at("type")!="message"||frame.at("v")!=5||frame.at("surface")!="service")return;
        const auto& e=frame.at("payload");if(e.at("type")!="mywallpaper.services"||e.at("v")!=1||frame.at("instanceId")!=e.at("generation"))return;
        if(e.at("kind")=="closed") {Revoke();return;}
        if(e.at("kind")!="packet")return;
        const auto& p=e.at("packet");const auto id=p.at("id").get<std::string>();if(!Token(p.at("id")))return;
        std::lock_guard lock(operationsMutex_);
        if(p.at("type")=="invoke.cancel") {if(auto it=operations_.find(id);it!=operations_.end())it->second->cancelled=1;return;}
        if(p.at("type")!="invoke"||operations_.contains(id))return;
        if(operations_.size()>=256)throw Failure("busy","Native invocation limit exceeded");
        auto op=std::make_shared<Operation>();
        op->deadline=std::chrono::steady_clock::now();
        // These fields are broker-owned, never accepted from payload.
        if(p.contains("connectionId")&&Token(p.at("connectionId"))&&p.contains("timeoutMs")&&p.at("timeoutMs").is_number_integer()&&p.contains("deadlineUnixMs")&&p.at("deadlineUnixMs").is_number_integer()) {
            auto nowUnix=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const auto timeout=p.at("timeoutMs").get<int64_t>(),expiry=p.at("deadlineUnixMs").get<int64_t>();
            if(timeout>=1&&timeout<=30000&&expiry>0&&expiry<=9007199254740991LL) {
                auto remaining=std::min<int64_t>(timeout,expiry>nowUnix?expiry-nowUnix:0);
                // The business timeout also begins at pipe arrival, not after
                // the UI queue. Never use payload identity as trusted context.
                if(p.value("method","")=="command"&&p.contains("payload")&&p.at("payload").is_object()) {
                    const auto& input=p.at("payload");
                    if(input.contains("timeoutMs")&&input.at("timeoutMs").is_number_integer()) {
                        const auto requested=input.at("timeoutMs").get<int64_t>();
                        if(requested>=1&&requested<=30000)remaining=std::min(remaining,requested);
                    }
                }
                op->trusted=true;op->connection=p.at("connectionId").get<std::string>();
                op->deadlineUnixMs=expiry;
                op->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max<int64_t>(0,remaining));
            }
        }
        operations_.emplace(id,std::move(op));
    } catch(const Failure&) {Revoke();throw;}
      catch(const std::exception&) { /* malformed packets fail at Receive, before dispatch */ }
}
void Provider::Revoke() noexcept {
    std::lock_guard lock(operationsMutex_);
    for(auto& [id,op]:operations_)op->cancelled=2;
}
void Provider::Envelope(const std::string& kind,const Json& fields) {
    Json envelope={{"type","mywallpaper.services"},{"v",1},{"generation",generation_},{"kind",kind}};
    envelope.update(fields);
    if(!FrameFits(envelope))throw std::runtime_error("Native service frame exceeds 64 KiB");
    send_(envelope);
}
void Provider::Packet(const Json& packet) {
    try {Envelope("packet",{{"packet",packet}});}
    catch(...) {Close("Native service transport failed; provider revoked");throw;}
}
Json Provider::Paged(const State& state) const {
    auto result=state.envelope;
    result["body"]={{"mode","paged"},{"inventoryId",state.inventory},{"totalBytes",state.bytes.size()},{"sha256",state.digest}};
    return result;
}
void Provider::Publish(const std::string& topic) {
    const auto& state=states_.at(topic);
    Packet({{"type","publish"},{"service","audio"},{"topic",topic},{"value",state.envelope}});
}
void Provider::Set(const std::string& topic,const std::string& availability,Json value,Json error) {
    if(!open_)return;
    auto& state=states_.at(topic);
    std::string bytes=value.is_null()?"":value.dump();
    const auto& other=states_.at(topic=="audio"?"media":"audio");
    std::string actualAvailability=availability;
    if(bytes.size()>inventoryLimit || bytes.size()+other.bytes.size()>inventoryLimit) {
        value=nullptr;bytes.clear();actualAvailability="unavailable";error=Error("message_too_large","Provider inventory budget exceeded; no data truncated");
    }
    Json next={{"kind","audio-system.state"},{"topic",topic},{"generation",generation_},{"revision",state.revision},
               {"availability",actualAvailability},{"error",error},{"body",{{"mode","inline"},{"value",value}}}};
    // Compare the original immutable value, not its inline/paged representation.
    if(state.bytes==bytes && state.envelope.value("availability","")==actualAvailability && state.envelope.value("error",Json())==error)return;
    state.revision++;next["revision"]=state.revision;
    state.bytes=std::move(bytes);state.digest=value.is_null()?"":Sha256(state.bytes);
    state.inventory="inventory-"+std::to_string(++inventorySequence_);state.envelope=std::move(next);
    Json wrapper={{"type","mywallpaper.services"},{"v",1},{"generation",generation_},{"kind","packet"},
                  {"packet",{{"type","publish"},{"service","audio"},{"topic",topic},{"value",state.envelope}}}};
    if(!FrameFits(wrapper))state.envelope=Paged(state);
    Validate(state.envelope,topic+"State");
    Publish(topic);
}
void Provider::AudioState(const Json& raw,bool enabled,bool allowMicrophone) {
    if(!open_)return;
    if(!enabled) {Set("audio","absent",nullptr);return;}
    try {
        Json value={{"endpoints",raw.at("endpoints")},{"sessions",raw.at("sessions")},{"errors",raw.at("errors")},
                    {"complete",raw.at("errors").empty()},{"restoration",raw.at("restoration")},{"runtime",raw.at("runtime")}};
        value["runtime"]["hwnd"]=std::to_string(raw.at("runtime").at("hwnd").get<uint64_t>());
        Unique(value.at("endpoints"));Unique(value.at("sessions"));
        std::set<std::string> endpoints;
        for(auto& endpoint:value["endpoints"]) {
            endpoints.insert(endpoint.at("id").get<std::string>());
            const bool writable=bool(execute_)&&(!endpoint.at("capture").get<bool>()||allowMicrophone);
            endpoint["capabilities"]={{"volume",writable},{"mute",writable}};
        }
        for(auto& session:value["sessions"]) {
            if(!endpoints.contains(session.at("endpointId").get<std::string>()))throw std::runtime_error("Orphaned audio session; projection incomplete");
            const auto endpoint=std::find_if(value["endpoints"].begin(),value["endpoints"].end(),[&](const auto& e){return e.at("id")==session.at("endpointId");});
            const bool writable=bool(execute_)&&!endpoint->at("capture").template get<bool>();
            session["capabilities"]={{"volume",writable},{"mute",writable}};
        }
        Validate(value,"audioValue");
        const bool empty=value["endpoints"].empty()&&value["sessions"].empty();
        if(empty&&!value["errors"].empty())throw std::runtime_error("Audio enumeration failed; empty is not established");
        Set("audio",empty?"empty":"ready",std::move(value));
    } catch(const std::exception& e) {Unavailable("audio",e.what());}
}
void Provider::MediaState(const Json& raw,bool enabled,bool collected) {
    if(!open_)return;
    if(!enabled || !collected) {Set("media","absent",nullptr);return;}
    try {
        const auto status=raw.at("status").get<std::string>();
        if(status=="unavailable") {Unavailable("media",raw.value("message","Media unavailable"));return;}
        if(status!="ready"&&status!="empty")throw std::runtime_error("Unknown media availability");
        Json value={{"sessions",raw.at("sessions")},{"selected",nullptr}};
        Unique(value.at("sessions"));
        for(auto& session:value["sessions"])session["capabilities"]=nullptr;
        if(status=="ready") {
            Json selected={{"id",raw.at("sessionId")}};
            for(const auto* key:{"source","title","artist","album","shuffle","repeat","playing","positionMs","durationMs","seekStartMs","seekEndMs"})selected[key]=raw.at(key);
            // Unknown sessions retain null; selected controls are effectively
            // unavailable until the command budget/identity gate is satisfied.
            Validate(raw.at("capabilities"),"mediaCapabilities");selected["capabilities"]=execute_?raw.at("capabilities"):NoMediaControls();
            bool found=false;for(auto& session:value["sessions"])if(session.at("id")==selected.at("id")) {session["capabilities"]=selected["capabilities"];found=true;}
            if(!found)throw std::runtime_error("Selected media session disappeared");
            if(selected.at("seekStartMs").get<int64_t>()>selected.at("seekEndMs").get<int64_t>())throw std::runtime_error("Invalid media seek range");
            value["selected"]=std::move(selected);
        }
        Validate(value,"mediaValue");Set("media",status,std::move(value));
    } catch(const std::exception& e) {Unavailable("media",e.what());}
}
void Provider::Unavailable(const std::string& topic,const std::string& message) {
    Set(topic,"unavailable",nullptr,Error("unavailable",message));
}
Json Provider::Snapshot(const Json& input) {
    Validate(input,"snapshotRequest");
    Json result={{"kind","audio-system.snapshot.result"},{"requestId",input.at("requestId")},{"generation",generation_},
                 {"audio",states_.at("audio").envelope},{"media",states_.at("media").envelope}};
    // Reserve full canonical response framing, not only the business payload.
    auto fits=[&]{return FrameFits({{"type","mywallpaper.services"},{"v",1},{"generation",generation_},{"kind","packet"},
        {"packet",{{"type","result"},{"id",std::string(160,'x')},{"ok",true},{"value",result}}}});};
    if(!fits()) {
        auto larger=states_.at("audio").bytes.size()>=states_.at("media").bytes.size()?"audio":"media";
        const auto& state=states_.at(larger);if(!state.bytes.empty())result[larger]=Paged(state);
        if(!fits())for(const auto* topic:{"audio","media"})if(!states_.at(topic).bytes.empty())result[topic]=Paged(states_.at(topic));
    }
    if(!fits())throw std::runtime_error("Snapshot framing exceeds limit");
    Validate(result,"snapshotResult");return result;
}
Json Provider::Inventory(const Json& input) {
    Validate(input,"inventoryRequest");
    auto failure=[&](const std::string& code){return Json{{"kind","audio-system.read.error"},{"requestId",input.at("requestId")},{"generation",generation_},{"error",Error(code,"Inventory identity or revision is no longer current")}};};
    if(input.at("generation")!=generation_)return failure("stale_generation");
    const auto& topic=input.at("topic").get_ref<const std::string&>();const auto& state=states_.at(topic);
    if(input.at("revision")!=state.revision || input.at("inventoryId")!=state.inventory || state.bytes.empty())return failure("inventory_expired");
    auto offset=input.at("offset").get<size_t>();if(offset>=state.bytes.size()||offset%chunkSize)return failure("invalid_request");
    auto bytes=state.bytes.substr(offset,chunkSize);auto next=offset+bytes.size();
    Json result={{"kind","audio-system.inventory.result"},{"requestId",input.at("requestId")},{"generation",generation_},{"topic",topic},
                 {"revision",state.revision},{"inventoryId",state.inventory},{"offset",offset},{"totalBytes",state.bytes.size()},{"sha256",state.digest},
                 {"encoding","base64-utf8-json"},{"data",Base64(bytes)},{"nextOffset",next==state.bytes.size()?Json(nullptr):Json(next)}};
    Validate(result,"inventoryResult");return result;
}
Json Provider::Command(const Json& input,Operation& operation) {
    Validate(input,"commandRequest");
    std::string failure="unsupported";
    try {
        if(!operation.trusted||!execute_)throw Failure("unsupported","No dispatch: missing trusted connection/deadline or native executor");
        auto key=std::make_pair(operation.connection,input.at("requestId").get<std::string>());
        if(requests_.contains(key))throw Failure("invalid_request","requestId already used on this connection/generation");
        const size_t bytes=key.first.size()+key.second.size();
        if(requestBytes_+bytes>inventoryLimit)throw Failure("busy","Request identity retention full; no ID evicted or replayed");
        requests_.insert(key);requestBytes_+=bytes;
        operation.Check();
        if(input.at("generation")!=generation_)throw Failure("stale_generation","Provider generation changed");
        const auto& command=input.at("command");const bool media=command.at("kind")=="media.command";
        const auto& state=states_.at(media?"media":"audio");
        if(input.at("revision")!=state.revision)throw Failure("stale_revision","Topic revision changed");
        if(state.envelope.at("availability")=="absent"||state.envelope.at("availability")=="unavailable")throw Failure("unavailable","No current usable state");
        const auto value=Json::parse(state.bytes);const auto action=command.at("action").get<std::string>();
        if(media && action!="refresh" && action!="session") {
            if(value.at("selected").is_null()||value.at("selected").at("id")!=input.at("expectedSessionId"))throw Failure("stale_session","Pinned media session changed");
            if(!value.at("selected").at("capabilities").at(action).get<bool>())throw Failure("unsupported","Media capability is absent");
            if(action=="seek"&&(command.at("value")<value.at("selected").at("seekStartMs")||command.at("value")>value.at("selected").at("seekEndMs")))throw Failure("invalid_request","Seek outside observed range");
        } else if(media && action=="session") {
            if(std::none_of(value.at("sessions").begin(),value.at("sessions").end(),[&](const auto& s){return s.at("id")==command.at("value");}))throw Failure("stale_session","Media session disappeared");
        } else if(!media&&action!="refresh"&&action!="show") {
            const auto& group=value.at(action.rfind("endpoint",0)==0?"endpoints":"sessions");
            auto item=std::find_if(group.begin(),group.end(),[&](const auto& s){return s.at("id")==command.at("id");});
            if(item==group.end())throw Failure("stale_session","Audio endpoint or session disappeared");
            if(!item->at("capabilities").at(action.ends_with("Volume")?"volume":"mute").get<bool>())throw Failure("permission_denied","Audio control is not currently writable");
        }
        operation.Check();execute_(command,input.at("expectedSessionId"),operation);operation.Check();
        return {{"kind","audio-system.command.result"},{"requestId",input.at("requestId")},{"generation",generation_},{"ok",true},{"outcome","acknowledged"},{"error",nullptr}};
    } catch(const Failure& e) {failure=e.code;}
      catch(const std::exception&) {failure="native_failure";}
    return {{"kind","audio-system.command.result"},{"requestId",input.at("requestId")},{"generation",generation_},{"ok",false},
            {"outcome",operation.dispatched?"unknown":"rejected"},{"error",Error(failure,"Native command refused or interrupted; no automatic retry")}};
}
Json Provider::Call(const Json& packet,Operation& operation) {
    if(packet.at("service")!="audio")throw std::runtime_error("Undeclared service");
    // Only the host's packet caller is accepted; payload-supplied identities are
    // rejected by the public schema, never consulted for authorization.
    const auto& caller=packet.at("caller");
    for(const auto* field:{"nodeId","addonId","generation"})if(!caller.contains(field)||!caller.at(field).is_string()||caller.at(field).get_ref<const std::string&>().empty())throw std::runtime_error("Missing host caller identity");
    const auto method=packet.at("method").get<std::string>();const auto& input=packet.at("payload");
    if(method=="command")return Command(input,operation);
    if(operation.trusted)operation.Check();
    if(method=="snapshot")return Snapshot(input);
    if(method=="inventory")return Inventory(input);
    throw std::runtime_error("Unknown audio-system method");
}
bool Provider::Receive(const Json& frame) {
    if(!frame.is_object()||frame.value("type","")!="message"||!frame.contains("payload")||!frame.at("payload").is_object()||frame.at("payload").value("type","")!="mywallpaper.services")return false;
    const auto& envelope=frame.at("payload");
    // Ignore non-service surface lookalikes; they cannot rotate a generation.
    if(frame.value("v",0)!=5||frame.value("surface","")!="service"||!envelope.contains("generation")||!Token(envelope.at("generation"))||frame.value("instanceId","")!=envelope.at("generation").get<std::string>())return true;
    if(frame.dump().size()>jsonLimit || envelope.value("v",0)!=1) {Close("Invalid native service envelope");return true;}
    const auto kind=envelope.value("kind","");
    const std::set<std::string> allowed=kind=="open"?std::set<std::string>{"type","v","generation","kind"}:kind=="closed"?std::set<std::string>{"type","v","generation","kind","reason"}:std::set<std::string>{"type","v","generation","kind","packet","deliveryId"};
    for(auto it=envelope.begin();it!=envelope.end();++it)if(!allowed.contains(it.key())) {Close("Unknown native service envelope field");return true;}
    if(kind=="open") {
        if(open_) {if(generation_!=envelope.at("generation").get<std::string>())Close("Overlapping service generation");return true;}
        generation_=envelope.at("generation").get<std::string>();open_=true;states_.clear();inventorySequence_=0;
        requests_.clear();requestBytes_=0;
        for(const auto* topic:{"audio","media"}) {
            State state;state.envelope={{"kind","audio-system.state"},{"topic",topic},{"generation",generation_},{"revision",0},{"availability","absent"},{"error",nullptr},{"body",{{"mode","inline"},{"value",nullptr}}}};
            states_.emplace(topic,std::move(state));
        }
        Packet({{"type","provide"},{"service","audio"}});Publish("audio");Publish("media");return true;
    }
    if(!open_)return true;
    if(envelope.at("generation")!=generation_) {Close("Revoked native service generation");return true;}
    if(kind=="closed") {Revoke();open_=false;states_.clear();requests_.clear();requestBytes_=0;std::lock_guard lock(operationsMutex_);operations_.clear();return true;}
    if(kind!="packet"||!envelope.contains("packet")||!envelope.at("packet").is_object()||!envelope.contains("deliveryId")||!Token(envelope.at("deliveryId"))) {Close("Expected acknowledged service delivery");return true;}
    // ACK releases transport bytes; it is not the invocation result.
    Envelope("ack",{{"deliveryId",envelope.at("deliveryId")}});
    const auto& packet=envelope.at("packet");const auto type=packet.value("type","");
    if(type=="invoke.cancel") {Prepare(frame);return true;}
    if(type=="closed") {Close("Broker closed native provider");return true;}
    if(type!="invoke"||!packet.contains("id")||!Token(packet.at("id"))) {Close("Unexpected provider packet");return true;}
    Prepare(frame);std::shared_ptr<Operation> operation;
    {std::lock_guard lock(operationsMutex_);operation=operations_.at(packet.at("id").get<std::string>());}
    try {Packet({{"type","result"},{"id",packet.at("id")},{"ok",true},{"value",Call(packet,*operation)}});}
    catch(const std::exception& e) {Packet({{"type","result"},{"id",packet.at("id")},{"ok",false},{"error",Error("invalid_request",e.what()).at("message")}});}
    {std::lock_guard lock(operationsMutex_);operations_.erase(packet.at("id").get<std::string>());}
    return true;
}
void Provider::Close(const std::string& reason) {
    if(!open_)return;
    Revoke();
    open_=false;states_.clear();requests_.clear();requestBytes_=0;
    {std::lock_guard lock(operationsMutex_);operations_.clear();}
    Envelope("closed",{{"reason",Error("revoked",reason).at("message")}});
}
}
