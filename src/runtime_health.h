#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace clashflux::health {
// Milliseconds from the adapter's monotonic clock, never wall-clock time.
using Tick = std::int64_t;
enum class State { Unsupported, Paused, Waiting, Fresh, Stale, Reconnecting, Blocked };
enum class Control { Unknown, Responsive, Unreachable, AuthenticationError, InvalidResponse };
enum class Failure { None, Transport, InvalidFrame, Authentication, Permission, Unsupported };
inline std::string_view Name(State value) {
    switch (value) {
    case State::Unsupported: return "unsupported";
    case State::Paused: return "paused";
    case State::Waiting: return "waiting";
    case State::Fresh: return "fresh";
    case State::Stale: return "stale";
    case State::Reconnecting: return "reconnecting";
    case State::Blocked: return "blocked";
    }
    return "unknown";
}
inline std::string_view Name(Control value) {
    switch (value) {
    case Control::Unknown: return "unknown";
    case Control::Responsive: return "responsive";
    case Control::Unreachable: return "unreachable";
    case Control::AuthenticationError: return "authentication-error";
    case Control::InvalidResponse: return "invalid-response";
    }
    return "unknown";
}
struct ChannelSnapshot {
    State state = State::Paused;
    Failure failure = Failure::None;
    bool connected = false;
    bool periodic = true;
    std::uint64_t generation = 0;
    std::uint64_t received = 0;
    std::uint64_t valid = 0;
    std::uint64_t consumed = 0;
    std::uint64_t invalid = 0;
    Tick validAgeMs = -1;
    unsigned repairs = 0;
    bool operator==(const ChannelSnapshot&) const = default;
};
struct Snapshot {
    std::uint64_t runtimeEpoch = 0;
    Control control = Control::Unknown;
    ChannelSnapshot traffic;
    ChannelSnapshot connections;
    bool operator==(const Snapshot&) const = default;
};
// Pure decision state; the owner serializes access and performs any I/O.
// A socket handshake is not a sample. Valid zero samples are heartbeats.
class Channel {
public:
    void Reset(Tick now, bool periodic = true, Tick interval = 1000) {
        *this = Channel{};
        active_ = true;
        periodic_ = periodic;
        interval_ = interval;
        graceUntil_ = now + 10000;
        ++generation_;
    }
    void Stop() { active_ = false; connected_ = false; ++generation_; }
    void Resume(Tick now) { graceUntil_ = now + 10000; consecutive_ = 0; }
    std::uint64_t Generation() const { return generation_; }
    void Reopen(Tick now) {
        connected_ = false;
        ++generation_;
        Resume(now);
    }
    bool Accepts(std::uint64_t generation) const { return active_ && generation == generation_; }
    void Open(std::uint64_t generation) { if (Accepts(generation)) connected_ = true; }
    void Error(std::uint64_t generation, Failure failure) {
        if (!Accepts(generation)) return;
        connected_ = false;
        consecutive_ = 0;
        failure_ = failure;
    }
    void Frame(std::uint64_t generation, Tick now, bool valid) {
        if (!Accepts(generation)) return;
        ++received_;
        if (!valid) { ++invalid_; consecutive_ = 0; failure_ = Failure::InvalidFrame; return; }
        if (lastValid_ < 0 || now - lastValid_ > Threshold()) consecutive_ = 0;
        lastValid_ = now;
        ++valid_;
        ++consecutive_;
        if (consecutive_ >= 2 && failure_ != Failure::Authentication && failure_ != Failure::Permission && failure_ != Failure::Unsupported) failure_ = Failure::None;
    }
    void Consumed() { consumed_ = valid_; }
    void Block(Failure failure) { failure_ = failure; }
    ChannelSnapshot Read(Tick now) const {
        State state = State::Waiting;
        if (!active_) state = State::Paused;
        else if (failure_ == Failure::Unsupported) state = State::Unsupported;
        else if (failure_ == Failure::Authentication || failure_ == Failure::Permission) state = State::Blocked;
        else if (!connected_) state = State::Reconnecting;
        else if (now >= graceUntil_ && periodic_ && (lastValid_ < 0 || now - lastValid_ > Threshold())) state = State::Stale;
        else if (lastValid_ >= 0 && consecutive_ >= 2) state = State::Fresh;
        return {state, failure_, connected_, periodic_, generation_, received_, valid_, consumed_, invalid_,
                lastValid_ < 0 ? -1 : std::max<Tick>(0, now - lastValid_), repairs_};
    }
    bool RepairDue(Tick now) const {
        if (Read(now).state != State::Stale || now < nextAttempt_) return false;
        return std::count_if(attempts_.begin(), attempts_.end(), [now](Tick t) { return t >= 0 && now - t < 300000; }) < 3;
    }
    // Reserve before probing: failed probes are also bounded and cannot storm.
    void ReserveRepair(Tick now) {
        attempts_[repairs_ % attempts_.size()] = now;
        ++repairs_;
        nextAttempt_ = now + std::min<Tick>(120000, 30000LL << std::min(2u, repairs_ - 1));
    }
private:
    Tick Threshold() const { return std::max<Tick>(5000, 3 * interval_); }
    bool active_ = false, connected_ = false, periodic_ = true;
    Tick interval_ = 1000, lastValid_ = -1, graceUntil_ = 0, nextAttempt_ = 0;
    std::uint64_t generation_ = 0, received_ = 0, valid_ = 0, consumed_ = 0, invalid_ = 0;
    unsigned consecutive_ = 0, repairs_ = 0;
    Failure failure_ = Failure::None;
    std::array<Tick, 3> attempts_{-1, -1, -1};
};
} // namespace clashflux::health
