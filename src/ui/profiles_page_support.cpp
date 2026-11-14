// profiles_page_support.cpp — 订阅页共享辅助与平台导入/刷新流程。
#include <huxerui/huxerui.h>
#include <huxerui/qrcode.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"
#include "task_bridge.h"
#if defined(__ANDROID__)
#include "android_profile_http.h"
#if !defined(NDEBUG)
#include <android/log.h>
#endif
#endif

import clashflux.model;
import clashflux.data;
import clashflux.pptp;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.vpn;

#include "profiles_page_shared.h"
#include "profiles_model.h"
#include "../profile_link.h"

namespace clashflux::ui::profile_detail {
huxerui::StringVariant profileTypeLabel(std::string_view type) {
    if (type == "remote") return Localized("远程订阅");
    if (type == "local") return Localized("本地文件");
    if (type == "pptp") return Localized("PPTP 内网连接");
    return type.empty() ? Localized("其他订阅")
                        : LocalizedFormat("其他 · {}", std::string(type));
}

// 分区标签栏用的标签（Compact 视口）：手机宽度放不下「PPTP 内网连接」
// 长名，第 4 个标签会被屏幕右缘截断半个字；标签本身仍可横向滚动，但默认
// 一行放全是手机端的基本要求。只收窄标签，卡片/表单继续用完整名称。
huxerui::StringVariant profileTypeTabLabel(std::string_view type) {
    if (type == "pptp") return "PPTP";
    return profileTypeLabel(type);
}

bool isNativeVpnType(std::string_view type) {
    return type == "pptp";
}

NativeProfileSupport profileSupport() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    return {.pptp = true};
#else
    return {};
#endif
}

// 页面/弹窗由视口决定，与操作系统无关。Compact（含缩小后的桌面窗口）进入
// 独立页面；Medium/Expanded 使用桌面弹窗。平台只控制表单能力。
void OpenProfileEditInfo(std::int64_t id, bool compact,
                         const ProfileEditLoader& load_edit_info,
                         const ProfileEditDialog& show_edit_info,
                         huxerui::State<std::int64_t> edit_page_id) {
    if (compact) {
        load_edit_info(id, [edit_page_id, id] { edit_page_id = id; });
        return;
    }
    show_edit_info(id);
}

// 单行截断（UTF-8 代码点安全）：超限截断加省略号。Text 默认按词换行且无
// 省略号能力，长 URL/名称会把卡片撑高——网格里统一截断保证卡片等高。
std::string truncateOneLine(const std::string& s, std::size_t maxCodePoints) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (count == maxCodePoints) return s.substr(0, i) + "…";
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t len =
            c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        i += std::min(len, s.size() - i);
        ++count;
    }
    return s;
}

huxerui::View ShareQrView(huxerui::qrcode::Symbol symbol) {
    return huxerui::qrcode::QRCode(std::move(symbol))
        .With(huxerui::Frame{.width = 300.0F, .height = 300.0F});
}

[[huxerui::composable]] huxerui::View QrsSequenceView(
    std::shared_ptr<const std::vector<std::string>> frames,
    huxerui::qrcode::Symbol first, FluxToastHandle toast) {
    const huxerui::TaskScope tasks = huxerui::UseTaskScope();
    auto current = huxerui::UseState(first);
    const auto error_text = huxerui::UseString(Localized("无法生成 QRS 分享码"));
    huxerui::Lifecycle([=]() -> std::function<void()> {
        if (frames->size() < 2) return [] {};
        const huxerui::TaskHandle handle = tasks.Launch([=]() -> huxerui::Task<void> {
            using Clock = std::chrono::steady_clock;
            try {
                std::size_t index = 0;
                while (true) {
                    const auto deadline = Clock::now() + std::chrono::milliseconds{350};
                    index = (index + 1) % frames->size();
                    auto next = first;
                    if (index != 0) {
                        next = co_await RunOnTaskThread([frames, index] {
                            return huxerui::qrcode::Encode((*frames)[index]);
                        });
                    }
                    if (const auto remaining = deadline - Clock::now();
                        remaining > Clock::duration::zero()) {
                        co_await huxerui::Delay(remaining);
                    }
                    current = std::move(next);
                }
            } catch (const std::exception&) {
                toast.Show(error_text);
            }
        });
        return [handle] { handle.Cancel(); };
    });
    return ShareQrView(current.Get());
}

// 把弹窗数字输入解析为秒/分钟：空/非法回落 fallback。
int parseNumber(const huxerui::TextEditingValue& v, int fallback) {
    const std::string t = v.text;
    int out = 0;
    const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
    if (ec != std::errc() || out < 0) return fallback;
    return out;
}

std::vector<std::string> parseRouteField(std::string_view text) {
    std::vector<std::string> routes;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find_first_of(",\n", begin);
        std::string route = std::string(text.substr(
            begin, end == std::string_view::npos ? std::string_view::npos
                                                   : end - begin));
        while (!route.empty() &&
               std::isspace(static_cast<unsigned char>(route.front()))) {
            route.erase(route.begin());
        }
        while (!route.empty() &&
               std::isspace(static_cast<unsigned char>(route.back()))) {
            route.pop_back();
        }
        if (!route.empty()) routes.push_back(std::move(route));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return routes;
}

std::string joinRoutes(const std::vector<std::string>& routes) {
    std::string out;
    for (const std::string& route : routes) {
        if (!out.empty()) out += ", ";
        out += route;
    }
    return out;
}

std::string serializePptpConfig(const pptp::PptpConfig& config) {
    return std::format("server={}\nusername={}\npassword={}\ntimeout={}\n"
                       "require_mppe={}\n",
                       config.server, config.username, config.password,
                       config.connectTimeoutSecs,
                       config.requireMppe ? "true" : "false");
}

std::optional<std::string> makePptpConfig(
    const huxerui::TextEditingValue& server,
    const huxerui::TextEditingValue& username,
    const huxerui::TextEditingValue& password,
    const huxerui::TextEditingValue& timeout, bool requireMppe,
    std::string& error) {
    const pptp::PptpConfig config{
        .server = server.text,
        .username = username.text,
        .password = password.text,
        .connectTimeoutSecs = parseNumber(timeout, 0),
        .requireMppe = requireMppe,
    };
    const std::string nativeConfig = serializePptpConfig(config);
    if (!pptp::ParsePptpConfig(nativeConfig, error)) return std::nullopt;
    error.clear();
    return nativeConfig;
}

huxerui::StringVariant pptpStateText(const store::PptpState& state) {
    if (state.state == vpn::ConnectionState::Failed && !state.error.empty())
        return LocalizedFormat("连接失败：{}", state.error);
    if (!state.toolsAvailable) return Localized("PPTP 引擎不可用");
    switch (state.state) {
    case vpn::ConnectionState::Connected:
        if (!state.interfaceName.empty() && !state.gateway.empty())
            return LocalizedFormat("已连接 · {} · 网关 {}", state.interfaceName,
                                   state.gateway);
        if (!state.interfaceName.empty())
            return LocalizedFormat("已连接 · {}", state.interfaceName);
        if (!state.gateway.empty())
            return LocalizedFormat("已连接 · 网关 {}", state.gateway);
        return Localized("已连接");
    case vpn::ConnectionState::Connecting: return Localized("连接中…");
    case vpn::ConnectionState::Failed:
        return state.error.empty() ? Localized("连接失败")
                                   : LocalizedFormat("连接失败：{}", state.error);
    case vpn::ConnectionState::Disabled: return Localized("已禁用");
    case vpn::ConnectionState::Idle: return Localized("未连接");
    }
    return Localized("未连接");
}

std::size_t ResponsiveProfilePageIndex(
    bool compact,
    huxerui::State<std::int64_t> edit_page_id,
    huxerui::State<bool> create_page_open,
    huxerui::State<std::int64_t> file_page_id) {
    if (!compact) return 0U;
    if (edit_page_id.Get() != 0) return 1U;
    if (create_page_open.Get()) return 2U;
    if (file_page_id.Get() != 0) return 3U;
    return 0U;
}
void OpenProfileCreate(bool compact, huxerui::State<bool> page,
                       huxerui::TaskScope tasks,
                       const std::function<void()>& dialog) {
    if (compact) {
        page = true;
        return;
    }
    tasks.Launch([dialog]() -> huxerui::Task<void> {
        co_await huxerui::Delay(std::chrono::duration<double>{0});
        dialog();
    });
}

// All platforms share the domain refresh transaction and bounded file downloader.
huxerui::Task<std::string> RefreshProfileForPlatform(
    std::shared_ptr<AppHttpClient> http, std::int64_t id) {
    static_cast<void>(http);
    co_return co_await RunOnTaskThread([id] {
        auto& profiles = store::profilesStore();
        return profiles.refresh(id) ? std::string{} : profiles.lastError();
    });
}

// 所有平台共用领域导入事务；下载后端在 API 层选择。弹窗只提交 request。
std::string NormalizeProfileImportLink(ProfileImportRequest& request) {
    if (!request.remote) return {};
    const auto scheme = request.url.substr(0, request.url.find(':'));
    if (profile_link::IsSupportedScheme(scheme)) {
        std::string error;
        auto link = profile_link::Parse(request.url, error);
        if (!link) return error;
        request.url = std::move(link->url);
        if (request.name.empty()) request.name = std::move(link->name);
    }
    return profile_link::IsHttpUrl(request.url)
        ? std::string{} : "订阅下载仅支持 HTTP 或 HTTPS URL";
}

huxerui::Task<ProfileImportResult> ImportProfileForPlatform(
    std::shared_ptr<AppHttpClient>, ProfileImportRequest request) {
    if (auto error = NormalizeProfileImportLink(request); !error.empty())
        co_return ProfileImportResult{0, std::move(error)};
    const ProfileImportResult result = co_await RunOnTaskThread(
        [request = std::move(request)] {
            auto& ps = store::profilesStore();
            const std::int64_t id =
                request.remote
                    ? ps.importUrl(request.name, request.url, request.options)
                    : request.local
                          ? ps.importFile(request.name, request.picked_path,
                                          request.options)
                          : ps.importNative(request.name,
                                            "pptp",
                                            request.options.nativeConfig,
                                            request.options.nativeRoutes,
                                            request.options);
            return ProfileImportResult{id, ps.lastError()};
        });
    co_return result;
}


// 单张订阅卡：纯视图（零弹窗 State；任务域由页面下发，菜单句柄由卡片自持有），
// 弹窗经 openXxx(id) 回调到页面级懒加载打开。

} // namespace clashflux::ui::profile_detail

namespace clashflux::ui {

// 保真度摘要（订阅内容变化后统一提示用）。编译失败 / 无降级都返回空串，调用方
// 据此决定要不要额外弹一条 toast。
std::string ProfileFidelitySummary(std::int64_t profileId) {
    try {
        return store::coreStore().fidelitySummaryForProfile(profileId);
    } catch (...) {
        return {};
    }
}

// Android 自动更新逐个调用领域刷新，保留 Provider 到期策略。
// 下载与提交在任务线程完成；失败通过应用模型报告，保留已接受记录。
huxerui::Task<int> AndroidRefreshProfilesDueOnce(
    std::shared_ptr<AppHttpClient> http,
    std::shared_ptr<ProfilesModel> profilesModel) {
    const std::vector<model::Profile> due = co_await RunOnTaskThread(
        [] { return store::profilesStore().dueForUpdate(); });
    int updated = 0;
    for (const auto& row : due) {
        const auto revision = clashflux::data::session().profileRevision(row.id);
        const auto current = clashflux::data::session().listProfiles();
        const auto found = std::ranges::find(current, row.id, &model::Profile::id);
        if (found == current.end() || *found != row) continue;
        const auto outcome = co_await RunOnTaskThread([id = row.id, revision] {
            auto& profiles = store::profilesStore();
            if (clashflux::data::session().profileRevision(id) != revision)
                return std::pair{false, std::string{"订阅在更新前已变化"}};
            const bool ok = profiles.refresh(id, false);
            return std::pair{ok, ok ? std::string{} : profiles.lastError()};
        });
        if (outcome.first) {
            ++updated;
            profilesModel->ClearProfileRefreshFailure(row.id);
        } else {
            profilesModel->ReportProfileRefreshFailure(
                row.id, row.name, outcome.second);
        }
    }
    co_return updated;
}

} // namespace clashflux::ui
