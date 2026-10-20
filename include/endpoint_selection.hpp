#pragma once
#include <string>
#include <vector>

namespace audio {
// Inventory values only: this resolver never opens a device or calls Windows.
struct EndpointOption {
    std::string id, name;
    bool capture{}, isDefault{};
};
class EndpointSelection {
public:
    enum class Issue { none, unavailable, preferredMissing, preferredAmbiguous, choiceMissing, choiceAmbiguous };
    explicit EndpointSelection(bool capture):capture_(capture) {}
    void Refresh(const std::vector<EndpointOption>& inventory,const std::string& preference);
    void Choose(const std::string& id) { choice_=id; }
    const std::string& Id() const { return id_; }
    Issue Status() const { return issue_; }
    std::string Diagnostic() const;
private:
    bool capture_;
    std::string preference_, choice_, id_;
    Issue issue_{Issue::unavailable};
};
}
