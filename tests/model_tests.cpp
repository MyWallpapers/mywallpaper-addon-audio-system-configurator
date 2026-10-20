#include "model.hpp"
#include "wire.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace audio;
void Require(bool condition) { if (!condition) throw std::runtime_error("contract assertion failed"); }
template<class F> void Reject(F operation) { bool rejected = false; try { operation(); } catch (...) { rejected = true; } Require(rejected); }
int main() {
 try {
    Require(Manifest().at("settings").size() <= 100);
    auto base = ParseConfig(Json::object());
    Require(!base.flag("hotkeys") && !base.flag("ruleEnabled") && !base.flag("allowMicrophoneChanges"));
    Json values = base.values;
    values["accentColor"] = "#00112280"; values["showMedia"] = false; values["preferredOutput"] = "Écran HDMI";
    auto hidden = ParseConfig(values); Require(hidden.text("accentColor") == "#00112280");
    values["showMedia"] = true; Require(ParseConfig(values).text("preferredOutput") == "Écran HDMI");
    for (const auto& definition : Manifest().at("settings")) {
        if (definition.at("type") == "section") continue;
        Json invalid = base.values; invalid[definition.at("id").get<std::string>()] = Json::array(); Reject([&] { ParseConfig(invalid); });
    }
    Reject([&]{ ParseConfig({{"panelWidth",100}}); });
    Reject([&]{ ParseConfig({{"position","unknown"}}); });
    Require(Percent({{"volume", 0}}) == 0 && Percent({{"volume",100}}) == 1);
    Reject([&]{Percent({{"volume",101}});}); Reject([&]{Percent({{"volume",true}});});
    Lease lease; Require(std::abs(*lease.Apply(.8f,.3f)-.3f)<.001f); Require(std::abs(*lease.Restore(.3f)-.8f)<.001f);
    lease.Apply(.7f,.2f); Require(!lease.Restore(.4f));
    lease.Apply(.6f,.3f); Require(!lease.Apply(.5f,.3f) && !lease.held);
    std::stringstream stream; Json frame={{"type","message"},{"payload",std::string(1024*1024+123,'x')}};
    wire::Write(stream,frame); Require(wire::Read(stream) == frame); Require(!wire::Read(stream));
    for (const std::string bad : {std::string("\0\0\0\0",4),std::string("\1\0\0\x80x",5),std::string("\1\0\0\x40x",5),std::string("\2\0\0\0x",5)}) {
      std::stringstream input(bad); Reject([&]{wire::Read(input);});
    }
    std::cout << "PASS settings validation, hidden choices, rule ownership, bounded chunks\n";
    return 0;
 } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
