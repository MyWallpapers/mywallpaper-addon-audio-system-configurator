#include "endpoint_selection.hpp"
#include <algorithm>

namespace audio {
void EndpointSelection::Refresh(const std::vector<EndpointOption>& inventory,const std::string& preference) {
    if(preference_!=preference) {preference_=preference;choice_.clear();id_.clear();}
    const auto previous=id_;
    id_.clear();
    auto matches=[&](const auto& endpoint) {
        return endpoint.capture==capture_ && !endpoint.id.empty() &&
            (!choice_.empty()?endpoint.id==choice_:endpoint.name==preference_);
    };
    if(!choice_.empty() || !preference_.empty()) {
        const auto count=std::count_if(inventory.begin(),inventory.end(),matches);
        if(count!=1) {
            issue_=!choice_.empty()?(count?Issue::choiceAmbiguous:Issue::choiceMissing):
                (count?Issue::preferredAmbiguous:Issue::preferredMissing);
            return;
        }
        id_=std::find_if(inventory.begin(),inventory.end(),matches)->id;
    } else {
        // Preserve an existing automatic choice; otherwise prefer Windows, then
        // the first available endpoint, only when no favorite was configured.
        auto find=[&](auto predicate) {
            return std::find_if(inventory.begin(),inventory.end(),[&](const auto& endpoint) {
                return endpoint.capture==capture_ && !endpoint.id.empty() && predicate(endpoint);
            });
        };
        auto selected=find([&](const auto& endpoint){return endpoint.id==previous;});
        if(selected==inventory.end())selected=find([](const auto& endpoint){return endpoint.isDefault;});
        if(selected==inventory.end())selected=find([](const auto&){return true;});
        if(selected==inventory.end()) {issue_=Issue::unavailable;return;}
        id_=selected->id;
    }
    // An ID must name exactly one endpoint, including across directions.
    if(std::count_if(inventory.begin(),inventory.end(),[&](const auto& endpoint){return endpoint.id==id_;})!=1) {
        id_.clear();issue_=Issue::choiceAmbiguous;return;
    }
    issue_=Issue::none;
}
std::string EndpointSelection::Diagnostic() const {
    const std::string label=capture_?"Microphone":"Sortie";
    const std::string choose=capture_?" Choisissez un microphone.":" Choisissez une sortie.";
    switch(issue_) {
        case Issue::none:return {};
        case Issue::preferredMissing:return label+" : favori introuvable (« "+preference_+" »)."+choose;
        case Issue::preferredAmbiguous:return label+" : plusieurs périphériques portent le nom « "+preference_+" »."+choose;
        case Issue::choiceMissing:return label+" : le périphérique sélectionné est indisponible."+choose;
        case Issue::choiceAmbiguous:return label+" : l’identifiant du périphérique est ambigu."+choose;
        case Issue::unavailable:return label+" : aucun périphérique disponible.";
    }
    return {};
}
}
