#include "wire.hpp"
#include <algorithm>
#include <stdexcept>
namespace audio::wire {
constexpr size_t chunk_limit = 1024 * 1024;
std::optional<Json> Read(std::istream& input) {
    std::string data;
    bool continuing = false;
    for (;;) {
        unsigned char header[4]{};
        input.read(reinterpret_cast<char*>(header), 4);
        if (input.gcount() == 0 && !continuing) return std::nullopt;
        if (input.gcount() != 4) throw std::runtime_error("En-tête process-v2 tronqué.");
        uint32_t raw = uint32_t(header[0]) | uint32_t(header[1]) << 8 | uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
        uint32_t kind = raw >> 30, size = raw & 0x3fffffff;
        if (size == 0 || size > chunk_limit || (!continuing && kind > 1) || (continuing && kind < 2))
            throw std::runtime_error("Fragment process-v2 invalide.");
        size_t offset = data.size();
        data.resize(offset + size);
        input.read(data.data() + offset, size);
        if (input.gcount() != size) throw std::runtime_error("Fragment process-v2 tronqué.");
        if (kind == 0 || kind == 3) return Json::parse(data);
        continuing = true;
    }
}
void Write(std::ostream& output, const Json& value) {
    const auto data = value.dump();
    for (size_t offset = 0; offset < data.size(); offset += chunk_limit) {
        auto size = std::min(chunk_limit, data.size() - offset);
        uint32_t kind = data.size() <= chunk_limit ? 0 : offset == 0 ? 1 : offset + size == data.size() ? 3 : 2;
        uint32_t raw = (kind << 30) | static_cast<uint32_t>(size);
        unsigned char header[4] = {static_cast<unsigned char>(raw), static_cast<unsigned char>(raw >> 8), static_cast<unsigned char>(raw >> 16), static_cast<unsigned char>(raw >> 24)};
        output.write(reinterpret_cast<char*>(header), 4);
        output.write(data.data() + offset, size);
    }
    output.flush();
    if (!output) throw std::runtime_error("Canal process-v2 fermé.");
}
}
