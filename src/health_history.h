#pragma once
#include "runtime_health.h"
#include "runtime_network_probe.h"
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

namespace clashflux::health {
// No strings supplied by the core, URLs, names, addresses or error text can
// enter the persisted record. Output is generated exclusively from typed fields.
struct HistorySample {
    std::uint64_t at = 0, session = 0, epoch = 0, instance = 0, revision = 0;
    unsigned lifecycle = 0; // 0 stopped, 1 starting, 2 running, 3 failed
    bool tunIntent = false, tunRunning = false, proxyIntent = false;
    Snapshot streams;
    network_probe::Result network;
    std::int64_t networkAgeMs = -1;
    unsigned pptpConnected = 0, pptpUnknown = 0;
};
class History {
public:
    // Worker-thread only for disk I/O. Bounded to 128 periodic + 64 incident
    // records; write failures retain dirty data and report failure, never touch DB.
    bool Store(const std::filesystem::path& directory, HistorySample sample);
    std::string Report(const std::filesystem::path& directory);
private:
    bool Load(const std::filesystem::path&);
    bool Flush();
    std::mutex mutex_;
    std::filesystem::path file_;
    std::deque<HistorySample> recent_, incidents_;
    bool loaded_ = false, dirty_ = false, persisted_ = true;
    std::uint64_t lastWrite_ = 0;
};
} // namespace clashflux::health
