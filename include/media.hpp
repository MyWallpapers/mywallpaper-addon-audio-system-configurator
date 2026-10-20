#pragma once
#include "model.hpp"
#include "service_provider.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
namespace audio {
class Media {
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
    std::string selected_;
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession Current(const std::string& pinned = {}, service::Operation* operation = nullptr);
public:
    Json Snapshot();
    void Command(const Json& command, const std::string& pinned = {}, service::Operation* operation = nullptr);
};
}
