#pragma once
#include "model.hpp"
#include <functional>
#include <map>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace audio::service {
std::string Sha256(const std::string& bytes);
void Validate(const Json& value, const std::string& definition);
struct Failure : std::runtime_error {
    std::string code;
    Failure(std::string code, const std::string& message):std::runtime_error(message),code(std::move(code)) {}
};
struct Operation {
    std::chrono::steady_clock::time_point deadline;
    int64_t deadlineUnixMs{};
    std::atomic_int cancelled{}; // 1: cancel, 2: EOF/closed/generation revoke
    bool dispatched{}; // only the execution thread writes this bit
    std::string connection;
    bool trusted{};
    void Check() const;
    void Dispatch() { Check();dispatched=true; }
};

// Owns only cached data and canonical native service packets. Never invokes OS APIs.
class Provider {
    struct State {
        Json envelope;
        std::string bytes, digest, inventory;
        uint64_t revision{};
    };
    std::map<std::string, State> states_;
    std::string generation_;
    bool open_{};
    uint64_t inventorySequence_{};
    std::function<void(const Json&)> send_;
    std::function<void(const Json&,const Json&,Operation&)> execute_;
    std::mutex operationsMutex_;
    std::map<std::string,std::shared_ptr<Operation>> operations_;
    std::set<std::pair<std::string,std::string>> requests_;
    size_t requestBytes_{};
    void Envelope(const std::string& kind, const Json& fields = Json::object());
    void Packet(const Json& packet);
    void Publish(const std::string& topic);
    Json Paged(const State& state) const;
    void Set(const std::string& topic, const std::string& availability, Json value, Json error = nullptr);
    Json Call(const Json& packet, Operation& operation);
    Json Command(const Json& input, Operation& operation);
    Json Snapshot(const Json& input);
    Json Inventory(const Json& input);
public:
    explicit Provider(std::function<void(const Json&)> send,
        std::function<void(const Json&,const Json&,Operation&)> execute = {});
    void SetExecutor(std::function<void(const Json&,const Json&,Operation&)> execute) { execute_=std::move(execute); }
    // Called immediately by the pipe reader, before bounded UI delivery. Does
    // not write stdout or mutate retained states. Cancel/EOF can interrupt waits.
    void Prepare(const Json& frame);
    void Revoke() noexcept;
    // Full host frame: verifies v5, service surface, instance and host generation.
    bool Receive(const Json& frame);
    void AudioState(const Json& raw, bool enabled, bool allowMicrophone);
    void MediaState(const Json& raw, bool enabled, bool collected);
    void Unavailable(const std::string& topic, const std::string& message);
    void Close(const std::string& reason);
    bool IsOpen() const { return open_; }
    const std::string& Generation() const { return generation_; }
};
}
