#include "model.hpp"
#include "manifest_data.hpp"
#include <algorithm>
#include <cmath>
#include <regex>
#include <stdexcept>
namespace audio {
const Json& Manifest() { static const Json value = Json::parse(manifest_data); return value; }
Config ParseConfig(const Json& input) {
    if (!input.is_object()) throw std::invalid_argument("Les réglages doivent être un objet.");
    Config result{Json::object()};
    for (const auto& def : Manifest().at("settings")) {
        if (def.at("type") == "section") continue;
        const auto id = def.at("id").get<std::string>();
        const auto value = input.contains(id) ? input.at(id) : def.at("default");
        const auto type = def.at("type").get<std::string>();
        bool valid = true;
        if (type == "boolean") valid = value.is_boolean();
        else if (type == "range") valid = value.is_number() && std::isfinite(value.get<double>()) &&
            value.get<double>() >= def.at("min").get<double>() && value.get<double>() <= def.at("max").get<double>() &&
            std::floor(value.get<double>()) == value.get<double>();
        else if (type == "select") {
            valid = false;
            for (const auto& option : def.at("options")) if (option.at("value") == value) valid = true;
        } else if (type == "string" || type == "color") {
            valid = value.is_string() && value.get_ref<const std::string&>().size() <= 4096;
            if (valid && type == "color") valid = std::regex_match(value.get<std::string>(), std::regex("#[0-9a-fA-F]{8}"));
        }
        if (!valid) throw std::invalid_argument("Réglage invalide : " + id);
        result.values[id] = value;
    }
    return result;
}
float Percent(const Json& command, const char* field) {
    const auto& value = command.at(field);
    if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0 || value.get<double>() > 100)
        throw std::invalid_argument("Volume attendu entre 0 et 100.");
    return value.get<float>() / 100.f;
}
std::string Text(const Json& command, const char* field, size_t max) {
    const auto& value = command.at(field);
    if (!value.is_string() || value.get_ref<const std::string&>().size() > max)
        throw std::invalid_argument(std::string("Texte invalide : ") + field);
    return value.get<std::string>();
}
std::optional<float> Lease::Restore(float observed) {
    const bool restore = held && std::abs(observed - written) < .002f;
    held = false;
    return restore ? std::optional<float>(original) : std::nullopt;
}
std::optional<float> Lease::Apply(float observed, float ceiling) {
    if (held && std::abs(observed - written) >= .002f) { held = false; return std::nullopt; }
    if (!held) { original = observed; held = true; }
    written = std::min(original, ceiling);
    if (std::abs(written - observed) < .002f) return std::nullopt;
    return written;
}
}
