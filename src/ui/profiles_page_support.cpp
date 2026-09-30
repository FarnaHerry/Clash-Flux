// profiles_page_support.cpp — 订阅页共享辅助与平台导入/刷新流程。
#include <huxerui/huxerui.h>
#include <qrcodegen.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.db;
import clashflux.openvpn;
import clashflux.pptp;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.vpn;

#include "profiles_page_shared.h"

namespace clashflux::ui::profile_detail {

huxerui::StringVariant profileTypeLabel(std::string_view type) {
    if (type == "remote") return Localized("远程订阅");
    if (type == "local") return Localized("本地文件");
    if (type == "pptp") return Localized("PPTP 内网连接");
    if (type == "openvpn") return Localized("OpenVPN 内网连接");
    return type.empty() ? Localized("其他订阅")
                        : LocalizedFormat("其他 · {}", std::string(type));
}

// 分区标签栏用的标签（Compact 视口）：手机宽度放不下「PPTP/OpenVPN 内网连接」
// 两种长名，第 4 个标签会被屏幕右缘截断半个字；标签本身仍可横向滚动，但默认
// 一行放全是手机端的基本要求。只收窄标签，卡片/表单继续用完整名称。
huxerui::StringVariant profileTypeTabLabel(std::string_view type) {
    if (type == "pptp") return "PPTP";
    if (type == "openvpn") return "OpenVPN";
    return profileTypeLabel(type);
}

bool isNativeVpnType(std::string_view type) {
    return type == "pptp" || type == "openvpn";
}

NativeProfileSupport profileSupport() {
#if defined(__ANDROID__)
    return {.openvpn = true};
#elif defined(__linux__)
    return {.pptp = true, .openvpn = true};
#elif defined(_WIN32)
    return {.pptp = true, .openvpn = true};
#elif defined(__APPLE__)
    return {.openvpn = true};
#else
    return {.openvpn = true};
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

// 订阅链接二维码画笔：白底 + 近黑模块（固定高对比，不随主题翻转，保证
// 扫码成功率）。模块居中（DialogCard 自带边距当静区）。
huxerui::CanvasPainter QrPainter(const std::string& text) {
    return [text](huxerui::PaintContext& paint, huxerui::Size size) {
        const float side = std::min(size.width, size.height);
        if (side <= 0.0F) return;
        qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize();
        const float cell = side / static_cast<float>(n);
        const float ox = (size.width - side) / 2.0F;
        const float oy = (size.height - side) / 2.0F;
        paint.DrawRect({ox, oy, side, side},
                       huxerui::Color::Rgb(255, 255, 255), {});
        const huxerui::Color moduleColor = huxerui::Color::Rgb(17, 17, 17);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                if (qr.getModule(x, y)) {
                    paint.DrawRect({ox + static_cast<float>(x) * cell,
                                    oy + static_cast<float>(y) * cell, cell,
                                    cell},
                                   moduleColor, {});
                }
            }
        }
    };
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

huxerui::StringVariant openVpnStateText(const store::OpenVpnState& state) {
    if (!state.toolsAvailable) return Localized("OpenVPN 引擎不可用");
    switch (state.state) {
    case vpn::ConnectionState::Connected:
        return state.interfaceName.empty()
                   ? Localized("已交给 sing-box")
                   : LocalizedFormat("已连接 · {}", state.interfaceName);
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
    huxerui::State<std::int64_t> file_page_id,
    huxerui::State<std::int64_t> rules_page_id) {
    if (!compact) return 0U;
    if (edit_page_id.Get() != 0) return 1U;
    if (create_page_open.Get()) return 2U;
    if (file_page_id.Get() != 0) return 3U;
    return rules_page_id.Get() != 0 ? 4U : 0U;
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

// HuxerUI HttpClient 订阅抓取：GET + UA + 全程超时，响应转 store::FetchedProfile。
// 必须在 UI 线程任务协程里 co_await（HTTP 自带平台异步通道，禁入阻塞线程池）。
huxerui::Task<store::FetchedProfile> AndroidFetchProfile(
    std::shared_ptr<AppHttpClient> http, std::string url,
    int timeoutSecs) {
    store::FetchedProfile fetched;
    if (!http) {
        fetched.error = "HTTP 服务不可用";
        co_return fetched;
    }
    huxerui::HttpRequest request;
    request.url = std::move(url);
    request.headers.push_back(
        huxerui::HttpHeader{"User-Agent", "clash-flux/0.1"});
    request.timeout = std::chrono::milliseconds{
        (timeoutSecs > 0 ? timeoutSecs : 60) * 1000};
    const std::string urlText = request.url;
    huxerui::HttpResult<huxerui::HttpResponse> result =
        co_await http->SendAsync(std::move(request));
    if (!result.Succeeded()) {
        // 失败时把可定位的信息一起带出来：HuxerUI 的 message 可能是空的
        // （真机实测就只剩一个"传输失败"），code 是稳定分类，url 用于区分
        // 是哪一个订阅源。
        const huxerui::HttpError& error = result.Error();
        fetched.error = std::format(
            "{}（code={}, url={}）",
            error.message.empty() ? "平台栈未给出错误详情" : error.message,
            static_cast<int>(error.code), urlText);
        co_return fetched;
    }
    huxerui::HttpResponse response = std::move(result).Value();
    fetched.status = response.status_code;
    fetched.ok = response.status_code >= 200 && response.status_code < 300;
    for (const auto& header : response.headers) {
        fetched.headers.emplace(header.name, header.value);
    }
    fetched.body.assign(
        reinterpret_cast<const char*>(response.body.data()),
        static_cast<std::size_t>(response.body.size()));
    co_return fetched;
}

// Android 订阅导入：store 建行 → 平台栈抓取 → store 落盘。返回新订阅 id
//（失败 0，错误经 profilesStore().lastError() 读取）。
huxerui::Task<std::int64_t> AndroidImportRemote(
    std::shared_ptr<AppHttpClient> http, const std::string& name,
    const std::string& url, db::Profile options) {
    const std::int64_t nid = co_await RunOnTaskThread(
        [name, url, options] { return store::profilesStore().createRemote(
                                   name, url, options); });
    if (nid == 0) co_return 0;
    const store::FetchedProfile fetched = co_await AndroidFetchProfile(
        std::move(http), url, options.timeoutSecs);
    const bool ok = co_await RunOnTaskThread(
        [nid, fetched] { return store::profilesStore().completeRemote(
                             nid, fetched, true); });
    co_return ok ? nid : 0;
}

// Android 订阅更新：按订阅行的 URL/超时经平台栈抓取后收尾；非 remote 行
// 回落阻塞路径（保留「本地导入的订阅不支持更新」等语义）。返回错误串。
huxerui::Task<std::string> AndroidRefreshRemote(
    std::shared_ptr<AppHttpClient> http, std::int64_t id) {
    const std::optional<db::Profile> row = co_await RunOnTaskThread(
        [id]() -> std::optional<db::Profile> {
            for (const auto& p : store::profilesStore().list()) {
                if (p.id == id) return p;
            }
            return std::nullopt;
        });
    if (!row || row->url.empty()) {
        const std::string err = co_await RunOnTaskThread([id]() -> std::string {
            auto& ps = store::profilesStore();
            return ps.refresh(id) ? "" : ps.lastError();
        });
        co_return err;
    }
    const store::FetchedProfile fetched = co_await AndroidFetchProfile(
        std::move(http), row->url, row->timeoutSecs);
    const bool ok = co_await RunOnTaskThread(
        [id, fetched] { return store::profilesStore().completeRemote(
                             id, fetched, false); });
    co_return ok ? "" : store::profilesStore().lastError();
}

// 更新入口只在这里做一次平台选择；卡片自身只持有 refresh(id) 这个统一动作。
void RefreshProfileForPlatform(
    std::int64_t id, const ProfileRefreshAction& http_refresh,
    const ProfileRefreshAction& desktop_refresh) {
#if defined(__ANDROID__)
    http_refresh(id);
#else
    desktop_refresh(id);
#endif
}

// 导入流程的网络差异也在平台函数内收束：Android remote 走平台 HttpClient，
// 桌面及本地/原生订阅走阻塞 store。弹窗只提交 request，不再判断平台。
#if defined(__ANDROID__)
huxerui::Task<ProfileImportResult> ImportProfileForPlatform(
    std::shared_ptr<AppHttpClient> http, ProfileImportRequest request) {
    if (request.remote) {
        const std::int64_t id = co_await AndroidImportRemote(
            std::move(http), request.name, request.url, request.options);
        co_return ProfileImportResult{
            id, id == 0 ? store::profilesStore().lastError() : ""};
    }
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
                                            request.pptp ? "pptp" : "openvpn",
                                            request.options.nativeConfig,
                                            request.options.nativeRoutes,
                                            request.options);
            return ProfileImportResult{id, ps.lastError()};
        });
    co_return result;
}
#else
huxerui::Task<ProfileImportResult> ImportProfileForPlatform(
    std::shared_ptr<AppHttpClient>, ProfileImportRequest request) {
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
                                            request.pptp ? "pptp" : "openvpn",
                                            request.options.nativeConfig,
                                            request.options.nativeRoutes,
                                            request.options);
            return ProfileImportResult{id, ps.lastError()};
        });
    co_return result;
}
#endif

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

// Android 订阅自动更新泵的一次迭代：任务线程列出到期订阅 → 逐个经 HuxerUI
// HttpClient 抓取 → store completeRemote 收尾。错误落在订阅行 error 字段。
huxerui::Task<int> AndroidRefreshProfilesDueOnce(
    std::shared_ptr<AppHttpClient> http) {
    const std::vector<db::Profile> due = co_await RunOnTaskThread(
        [] { return store::profilesStore().dueForUpdate(); });
    int updated = 0;
    for (const auto& row : due) {
        const store::FetchedProfile fetched = co_await
            profile_detail::AndroidFetchProfile(http, row.url, row.timeoutSecs);
        const bool ok = co_await RunOnTaskThread(
            [id = row.id, fetched] {
                return store::profilesStore().completeRemote(id, fetched, false);
            });
        if (ok) ++updated;
    }
    co_return updated;
}

} // namespace clashflux::ui
