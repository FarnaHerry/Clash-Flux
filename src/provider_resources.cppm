// Application-owned request coordination. No compiler IO, persistent cache,
// detached tasks or publication of candidate/configuration state lives here.
export module clashflux.provider_resources;
import std;

namespace provider_resources {

export struct Request {
    // Full identity, never the filename digest; credentials stay in memory.
    std::string identity, url;
    std::map<std::string, std::string> headers;
    std::size_t maxBytes = 8 * 1024 * 1024;
    long timeoutSecs = 20;
    unsigned attempts = 1;
    bool acceptNotModified = false;
    bool allowInvalidCert = false, allowProxyEnv = false;
    std::string proxyUrl;
    auto operator<=>(const Request&) const = default;
};

export struct Response {
    bool ok = false;
    long status = 0;
    std::string body, error, etag, lastModified;
    std::int64_t networkMicros = 0;
    std::uint64_t bytes = 0;
    unsigned attempts = 0;
    bool connectionsKnown = false;
    long newConnections = 0;
};

export struct Outcome {
    std::shared_ptr<const Response> response;
    bool joined = false, cancelled = false;
    std::int64_t queueMicros = 0, sharedWaitMicros = 0, fetchMicros = 0;
};

export struct Snapshot {
    std::size_t active = 0, inFlight = 0;
    std::uint64_t joins = 0;
};

export class Manager {
    struct Flight {
        std::shared_ptr<Response> response = std::make_shared<Response>();
        bool done = false, abandoned = false;
    };
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::map<Request, std::shared_ptr<Flight>> flights_;
    std::size_t active_ = 0;
    std::uint64_t joins_ = 0;
    const std::size_t concurrency_, capacity_;

public:
    explicit Manager(std::size_t concurrency = 3, std::size_t capacity = 64)
        : concurrency_(std::clamp<std::size_t>(concurrency, 1, 4)),
          capacity_(std::clamp<std::size_t>(capacity, concurrency_, 256)) {}
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return {active_, flights_.size(), joins_};
    }

    Outcome fetch(const Request& request, const std::function<Response(const Request&)>& load,
                  std::stop_token cancel = {}) {
        using Clock = std::chrono::steady_clock;
        const auto elapsed = [](Clock::time_point began) {
            return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count();
        };
        Outcome outcome;
        if (cancel.stop_requested()) { outcome.cancelled = true; return outcome; }
        if (request.maxBytes == 0 || request.maxBytes > 64 * 1024 * 1024) {
            auto response = std::make_shared<Response>();
            response->error = "provider 大小上限无效";
            outcome.response = std::move(response);
            return outcome;
        }
        std::shared_ptr<Flight> flight;
        for (;;) {
            std::unique_lock lock(mutex_);
            if (cancel.stop_requested()) { outcome.cancelled = true; return outcome; }
            if (const auto found = flights_.find(request); found != flights_.end()) {
                flight = found->second;
                outcome.joined = true;
                ++joins_;
                const auto began = Clock::now();
                const bool completed = changed_.wait(lock, cancel, [&] { return flight->done; });
                outcome.sharedWaitMicros += elapsed(began);
                if (!completed || cancel.stop_requested()) {
                    outcome.cancelled = true;
                    return outcome;
                }
                // A cancelled queued owner performed no IO. One surviving
                // consumer takes over with its own loader/staging lifetime.
                if (flight->abandoned) continue;
                outcome.response = flight->response;
                return outcome;
            }
            if (flights_.size() >= capacity_) {
                auto response = std::make_shared<Response>();
                response->error = "provider 下载队列已满";
                outcome.response = std::move(response);
                return outcome;
            }
            flight = std::make_shared<Flight>();
            flights_.emplace(request, flight);
            outcome.joined = false;
            const auto queued = Clock::now();
            const bool available = changed_.wait(lock, cancel, [&] { return active_ < concurrency_; });
            outcome.queueMicros += elapsed(queued);
            if (!available || cancel.stop_requested()) {
                flight->done = flight->abandoned = true;
                flights_.erase(request);
                lock.unlock();
                changed_.notify_all();
                outcome.cancelled = true;
                return outcome;
            }
            ++active_;
            break;
        }
        // Running IO stays on its owner's task thread until completion. An
        // owner's cancellation cannot discard data needed by another candidate.

        const auto began = Clock::now();
        try {
            *flight->response = load(request);
            if (flight->response->body.size() > request.maxBytes) {
                flight->response->ok = false;
                flight->response->body.clear();
                flight->response->error = "provider 响应超过大小上限";
            }
        } catch (...) {
            flight->response->ok = false;
            flight->response->body.clear();
            flight->response->error = "provider 下载任务异常";
        }
        outcome.fetchMicros = elapsed(began);
        std::unique_lock lock(mutex_);
        --active_;
        flight->done = true;
        // Completed results are held only by current consumers. A later force
        // refresh must start new IO, including after a failed/throwing request.
        flights_.erase(request);
        lock.unlock();
        changed_.notify_all();
        outcome.cancelled = cancel.stop_requested();
        outcome.response = flight->response;
        return outcome;
    }
};

} // namespace provider_resources
