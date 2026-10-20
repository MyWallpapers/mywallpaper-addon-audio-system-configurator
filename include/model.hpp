#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
namespace audio {
using Json = nlohmann::json;
const Json& Manifest();
struct Config {
    Json values;
    bool flag(const char* id) const { return values.at(id).get<bool>(); }
    int number(const char* id) const { return values.at(id).get<int>(); }
    std::string text(const char* id) const { return values.at(id).get<std::string>(); }
};
Config ParseConfig(const Json& input);
float Percent(const Json& command, const char* field = "volume");
std::string Text(const Json& command, const char* field, size_t max = 4096);
struct Lease {
    float original{}, written{};
    bool held{};
    // External changes revoke the lease. A caller must not recapture it until
    // the same rule activation has ended (suppressed tracked by runtime).
    std::optional<float> Restore(float observed);
    std::optional<float> Apply(float observed, float ceiling);
};
}
