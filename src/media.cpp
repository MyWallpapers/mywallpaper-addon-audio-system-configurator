#include "media.hpp"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <chrono>
#include <stdexcept>
namespace audio {
using namespace winrt::Windows::Media::Control;
using namespace std::chrono_literals;
template<class T> static auto Await(T operation,service::Operation* budget=nullptr) {
    const auto deadline=std::chrono::steady_clock::now()+1500ms;
    try {
        for(;;) {
            if(budget)budget->Check();
            if(operation.wait_for(5ms)==winrt::Windows::Foundation::AsyncStatus::Completed)return operation.GetResults();
            if(std::chrono::steady_clock::now()>=deadline)throw service::Failure("timeout","La session média ne répond pas (1,5 s).");
        }
    } catch(...) {try{operation.Cancel();}catch(...){}throw;}
}
GlobalSystemMediaTransportControlsSession Media::Current(const std::string& pinned,service::Operation* operation) {
    if(operation)operation->Check();
    if (!manager_) manager_ = Await(GlobalSystemMediaTransportControlsSessionManager::RequestAsync(),operation);
    if(!pinned.empty()) {
        GlobalSystemMediaTransportControlsSession found{nullptr};
        for(const auto& session:manager_.GetSessions())if(winrt::to_string(session.SourceAppUserModelId())==pinned) {
            if(found)throw service::Failure("stale_session","Identité média ambiguë.");found=session;
        }
        if(!found)throw service::Failure("stale_session","La session média épinglée a disparu.");
        return found;
    }
    if (!selected_.empty()) {
        for (const auto& session : manager_.GetSessions()) if (winrt::to_string(session.SourceAppUserModelId()) == selected_) return session;
        selected_.clear();
    }
    return manager_.GetCurrentSession();
}
Json Media::Snapshot() {
    auto session = Current();
    Json result={{"kind","media.state"},{"status","empty"},{"sessions",Json::array()}};
    for (const auto& item : manager_.GetSessions()) {
        const auto id=winrt::to_string(item.SourceAppUserModelId()); result["sessions"].push_back({{"id",id},{"label",id}});
    }
    if (!session) return result;
    auto info = session.GetPlaybackInfo(); auto controls = info.Controls();
    auto properties = Await(session.TryGetMediaPropertiesAsync());
    auto timeline = session.GetTimelineProperties();
    auto ms=[](auto value){return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();};
    result.update({{"status","ready"},{"sessionId",winrt::to_string(session.SourceAppUserModelId())},{"source",winrt::to_string(session.SourceAppUserModelId())},
      {"title",winrt::to_string(properties.Title())},{"artist",winrt::to_string(properties.Artist())},{"album",winrt::to_string(properties.AlbumTitle())},
      {"shuffle",info.IsShuffleActive() ? info.IsShuffleActive().Value() : false},{"repeat",info.AutoRepeatMode() && info.AutoRepeatMode().Value()==winrt::Windows::Media::MediaPlaybackAutoRepeatMode::Track ? "track" : info.AutoRepeatMode() && info.AutoRepeatMode().Value()==winrt::Windows::Media::MediaPlaybackAutoRepeatMode::List ? "list" : "none"},{"playing",info.PlaybackStatus()==GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing},
      {"positionMs",ms(timeline.Position())},{"durationMs",ms(timeline.EndTime())},
      {"seekStartMs",ms(timeline.MinSeekTime())},{"seekEndMs",ms(timeline.MaxSeekTime())},
      {"capabilities",{{"playPause",controls.IsPlayPauseToggleEnabled()},{"previous",controls.IsPreviousEnabled()},{"next",controls.IsNextEnabled()},{"seek",controls.IsPlaybackPositionEnabled()},{"shuffle",controls.IsShuffleEnabled()},{"repeat",controls.IsRepeatEnabled()}}}});
    return result;
}
void Media::Command(const Json& command,const std::string& pinned,service::Operation* operation) {
    if(operation)operation->Check();
    auto dispatch=[&]{if(operation)operation->Dispatch();};
    const auto action = Text(command,"action",64);
    if (action == "refresh") {dispatch();manager_ = nullptr; (void)Current({},operation); return; }
    if (action == "session") {
        auto wanted=Text(command,"value"); (void)Current(wanted,operation);
        for (const auto& session : manager_.GetSessions()) if (winrt::to_string(session.SourceAppUserModelId()) == wanted) {dispatch();selected_=wanted; return; }
        throw std::runtime_error("Cette session média a disparu.");
    }
    auto session=Current(pinned,operation); if (!session) throw std::runtime_error("Aucune session média Windows.");
    auto controls=session.GetPlaybackInfo().Controls(); bool accepted=false;
    if (action=="playPause" && controls.IsPlayPauseToggleEnabled()) {dispatch();accepted=Await(session.TryTogglePlayPauseAsync(),operation);}
    else if (action=="previous" && controls.IsPreviousEnabled()) {dispatch();accepted=Await(session.TrySkipPreviousAsync(),operation);}
    else if (action=="next" && controls.IsNextEnabled()) {dispatch();accepted=Await(session.TrySkipNextAsync(),operation);}
    else if (action=="seek" && controls.IsPlaybackPositionEnabled()) {
        const auto& value=command.at("value");
        if (!value.is_number_integer()) throw std::invalid_argument("Position en millisecondes attendue.");
        auto position=value.get<int64_t>(); auto timeline=session.GetTimelineProperties();
        auto low=std::chrono::duration_cast<std::chrono::milliseconds>(timeline.MinSeekTime()).count();
        auto high=std::chrono::duration_cast<std::chrono::milliseconds>(timeline.MaxSeekTime()).count();
        if(position<low || position>high || position>INT64_MAX/10000) throw std::invalid_argument("Position hors de la piste.");
        dispatch();accepted=Await(session.TryChangePlaybackPositionAsync(position*10000),operation);
    } else if(action=="shuffle" && controls.IsShuffleEnabled()) {dispatch();accepted=Await(session.TryChangeShuffleActiveAsync(command.at("value").get<bool>()),operation);}
    else if(action=="repeat" && controls.IsRepeatEnabled()) {
        using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
        auto value=Text(command,"value",16);
        if(value!="none"&&value!="track"&&value!="list")throw std::invalid_argument("Répétition invalide.");
        dispatch();accepted=Await(session.TryChangeAutoRepeatModeAsync(value=="track"?MediaPlaybackAutoRepeatMode::Track:value=="list"?MediaPlaybackAutoRepeatMode::List:MediaPlaybackAutoRepeatMode::None),operation);
    } else throw std::runtime_error("Cette commande n’est pas disponible pour le lecteur sélectionné.");
    if (!accepted) throw std::runtime_error("Le lecteur a refusé la commande média.");
}
}
