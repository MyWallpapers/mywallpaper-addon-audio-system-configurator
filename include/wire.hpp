#pragma once
#include "model.hpp"
#include <istream>
#include <ostream>
namespace audio::wire {
std::optional<Json> Read(std::istream& input);
void Write(std::ostream& output, const Json& value);
}
