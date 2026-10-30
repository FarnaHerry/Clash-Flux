#pragma once

#include <mutex>
#include <stdexcept>

namespace clashflux::persistence {
// A pre-hydration read must not permanently publish an empty owner cache.
// Failed readiness/decoding leaves call_once retryable without mutating storage.
class HydratedOnce {
public:
    template<class Ready, class Load>
    void Run(Ready ready, Load load) {
        std::call_once(once_, [&] {
            if (!ready()) throw std::runtime_error("持久化尚未加载，稍后重试");
            load();
        });
    }
private:
    std::once_flag once_;
};
} // namespace clashflux::persistence
