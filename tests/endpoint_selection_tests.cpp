#include "endpoint_selection.hpp"
#include <iostream>
#include <stdexcept>

using audio::EndpointOption;
using audio::EndpointSelection;
using Issue=EndpointSelection::Issue;
namespace {
void Require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void Resolved(const EndpointSelection& selection,const std::string& id) {
    Require(selection.Id()==id,"wrong endpoint ID");
    Require(selection.Status()==Issue::none && selection.Diagnostic().empty(),"resolved endpoint has a diagnostic");
}
void Blocked(const EndpointSelection& selection,Issue issue) {
    Require(selection.Id().empty(),"unresolved selection must have no mutation target");
    Require(selection.Status()==issue && !selection.Diagnostic().empty(),"unresolved selection needs a visible diagnostic");
}
}
int main() try {
    unsigned scenarios=0;
    for(bool capture:{false,true}) {
        const std::vector<EndpointOption> inventory={
            {"other-direction","Favorite",!capture,true},
            {"windows","Windows default",capture,true},
            {"favorite","Favorite",capture,false},
            {"manual","Manual choice",capture,false}
        };
        // A non-default favorite wins, with direction filtering.
        EndpointSelection present(capture);
        present.Refresh(inventory,"Favorite");Resolved(present,"favorite");++scenarios;

        EndpointSelection missing(capture);
        missing.Refresh(inventory,"Missing");Blocked(missing,Issue::preferredMissing);++scenarios;

        auto duplicate=inventory;
        duplicate.push_back({"duplicate","Favorite",capture,true});
        EndpointSelection ambiguous(capture);
        ambiguous.Refresh(duplicate,"Favorite");Blocked(ambiguous,Issue::preferredAmbiguous);++scenarios;

        EndpointSelection automatic(capture);
        automatic.Refresh(inventory,"");Resolved(automatic,"windows");
        auto changedDefault=inventory;
        changedDefault[1].isDefault=false;changedDefault[2].isDefault=true;
        automatic.Refresh(changedDefault,"");Resolved(automatic,"windows");++scenarios;

        EndpointSelection noDefault(capture);
        auto noWindowsDefault=inventory;noWindowsDefault[1].isDefault=false;
        noDefault.Refresh(noWindowsDefault,"");Resolved(noDefault,"windows");++scenarios;

        // Every refresh rechecks the name, rather than retaining a cached ID.
        auto disappeared=inventory;disappeared.erase(disappeared.begin()+2);
        present.Refresh(disappeared,"Favorite");Blocked(present,Issue::preferredMissing);
        present.Refresh(inventory,"Favorite");Resolved(present,"favorite");
        auto renamed=inventory;renamed[2].name="Different device";
        present.Refresh(renamed,"Favorite");Blocked(present,Issue::preferredMissing);++scenarios;

        // Ambiguity introduced after a successful resolution invalidates it.
        present.Refresh(inventory,"Favorite");Resolved(present,"favorite");
        present.Refresh(duplicate,"Favorite");Blocked(present,Issue::preferredAmbiguous);++scenarios;

        // An explicit list choice can resolve both missing and ambiguous names.
        missing.Choose("manual");missing.Refresh(inventory,"Missing");Resolved(missing,"manual");
        ambiguous.Choose("manual");ambiguous.Refresh(duplicate,"Favorite");Resolved(ambiguous,"manual");++scenarios;

        // A disappeared explicit choice must not switch to a favorite/default.
        auto lostManual=inventory;lostManual.pop_back();
        ambiguous.Refresh(lostManual,"Favorite");Blocked(ambiguous,Issue::choiceMissing);
        ambiguous.Refresh(lostManual,"Favorite");Blocked(ambiguous,Issue::choiceMissing);
        ambiguous.Choose("windows");ambiguous.Refresh(lostManual,"Favorite");Resolved(ambiguous,"windows");++scenarios;

        // Changing the configured preference clears the earlier manual choice.
        missing.Refresh(inventory,"Favorite");Resolved(missing,"favorite");
        missing.Refresh(inventory,"Missing again");Blocked(missing,Issue::preferredMissing);++scenarios;

        // Even a delayed choice from an open list is checked against new inventory.
        present.Choose("favorite");present.Refresh(disappeared,"Favorite");Blocked(present,Issue::choiceMissing);
        present.Choose("manual");present.Refresh(disappeared,"Favorite");Resolved(present,"manual");++scenarios;

        auto duplicateId=inventory;duplicateId.push_back({"favorite","Other name",!capture,false});
        EndpointSelection invalidId(capture);
        invalidId.Refresh(duplicateId,"Favorite");Blocked(invalidId,Issue::choiceAmbiguous);++scenarios;

        automatic.Refresh({},"");Blocked(automatic,Issue::unavailable);
        automatic.Refresh(inventory,"");Resolved(automatic,"windows");++scenarios;

        EndpointSelection wrongDirection(capture);
        wrongDirection.Refresh({inventory.front(),inventory[1]},"Favorite");Blocked(wrongDirection,Issue::preferredMissing);++scenarios;

        EndpointSelection manualWithoutFavorite(capture);
        manualWithoutFavorite.Refresh(inventory,"");manualWithoutFavorite.Choose("manual");
        manualWithoutFavorite.Refresh(inventory,"");Resolved(manualWithoutFavorite,"manual");
        manualWithoutFavorite.Refresh(lostManual,"");Blocked(manualWithoutFavorite,Issue::choiceMissing);++scenarios;
    }
    std::cout<<"PASS "<<scenarios<<" simulated endpoint-selection scenarios; no Windows/audio API calls\n";
    return 0;
} catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
}
