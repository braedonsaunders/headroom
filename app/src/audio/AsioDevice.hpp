#pragma once
#include "AudioIO.hpp"

namespace rig::audio {

// ASIO has no callback user-data argument. One stream owns the process-wide
// callback target until stop/dispose has returned on its COM apartment.
class AsioDevice {
public:
    struct Status {
        bool running = false;
        int inputs = 0;
        int frames = 0;
        double rate = 0;
        double latencyMs = 0;
    };
    using StatusFn = std::function<void(const Status&)>;
    AsioDevice();
    ~AsioDevice();
    static std::vector<std::string> drivers();
    bool start(const AudioSettings&, bool live, RenderFn, StatusFn,
               std::atomic<uint64_t>& overloads, std::string* err);
    void stop();
    std::string activeName() const;
    std::string error() const;
    int inputChannels() const;
    bool openPanel(std::string* err);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rig::audio
