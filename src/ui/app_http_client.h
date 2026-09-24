#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <huxerui/huxerui.h>

namespace clashflux::ui {

// One HTTP transport handle per HuxerUI Runtime. HttpClient is bound to the
// Runtime that constructs it, so keep the shared instance in application
// services instead of a process-wide static.
class AppHttpClient final {
public:
    [[nodiscard]] huxerui::Task<
        huxerui::HttpResult<huxerui::HttpResponse>>
    SendAsync(huxerui::HttpRequest request,
              std::function<void(huxerui::HttpProgress)> progress = {}) const {
        return client_.SendAsync(std::move(request), std::move(progress));
    }

private:
    huxerui::HttpClient client_;
};

inline void InstallAppHttpClient(huxerui::ApplicationContext& context) {
    context.Provide(std::make_shared<AppHttpClient>());
}

} // namespace clashflux::ui
