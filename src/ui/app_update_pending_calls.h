#pragma once

#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// PlatformChannel drops callbacks on closure. This feature-owned ledger settles
// still-live application Tasks when their original presentation session closes.
class AppUpdatePendingCalls {
public:
    using Completion = huxerui::CallbackCompletion<huxerui::PlatformPayload>;
    std::uint64_t Add(Completion completion) {
        if (closed_) {
            completion.Fail(ClosedError());
            return 0;
        }
        const auto token = ++serial_;
        pending_.emplace(token, std::move(completion));
        return token;
    }
    void Abandon(std::uint64_t token) { pending_.erase(token); }
    void Finish(std::uint64_t token, huxerui::PlatformResult<huxerui::PlatformPayload> result) {
        const auto found = pending_.find(token);
        if (found == pending_.end()) return;
        const auto reply = found->second;
        pending_.erase(found);
        if (const auto* error = std::get_if<huxerui::PlatformError>(&result))
            reply.Fail(std::make_exception_ptr(std::runtime_error(error->message)));
        else reply.Complete(std::get<huxerui::PlatformPayload>(std::move(result)));
    }
    void Close() {
        if (closed_) return;
        closed_ = true;
        auto waiting = std::move(pending_);
        pending_.clear();
        for (const auto& [token, reply] : waiting) reply.Fail(ClosedError());
    }
private:
    static std::exception_ptr ClosedError() {
        return std::make_exception_ptr(std::runtime_error("Android update window or module was closed"));
    }
    bool closed_ = false;
    std::uint64_t serial_ = 0;
    std::unordered_map<std::uint64_t, Completion> pending_;
};

} // namespace clashflux::ui
