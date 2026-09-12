// profiles_page.cpp — 订阅页：统一尺寸矩形卡片网格（定宽定高，内容单行
// UTF-8 截断；Compact 视口退化为整宽列表）。卡片交互：右上角刷新图标更新
// 订阅；右键弹上下文菜单（使用/更新/首页/分享二维码/编辑信息/编辑规则/
// 编辑文件/删除）；双击卡片切换启用订阅。
//
// 订阅选项（类型/描述/HTTP 超时/更新间隔/自动更新/系统代理/内核代理/无效证书）
// 在新建与编辑弹窗编辑，仅落库，下载行为在下次「更新」时生效；自动更新由
// 壳层泵（app.cpp）按间隔扫描 refreshDue()。
//
// 性能（懒加载）：弹窗相关 State 全部收在页面级（每张卡零弹窗状态，只有
// UseTheme；菜单句柄也由页面下发），卡片的组合成本与弹窗数量解耦；编辑
// 文件的 YAML 经任务线程读取（加载指示），TextField 非受控——OnChanged
// 只写 shared_ptr 指向的内容（无 State 写 → 无逐键重组），保存时取现值。
//
// 数据流：列表经 2s 泵从 profilesStore 重读（未变化时 State 相等短路，无
// 重组）；导入/更新/启用/删除/编辑保存都是阻塞活（网络下载 / 内核重启），
// 全部经 RunOnTaskThread。
#include <huxerui/huxerui.h>

#include <qrcodegen.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.db;
import clashflux.openvpn;
import clashflux.pptp;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.utils;
import clashflux.vpn;

namespace clashflux::ui {
namespace {

// 卡片统一尺寸：定宽（Flow 网格换行）+ 定高（内容单行截断，ClipChildren
// 兜底）；Compact 视口整宽（高度仍统一）。
constexpr float kCardWidth = 280.0F;
constexpr float kCardHeight = 180.0F;

// 弹窗表单区滚动视口高度：字段多（类型/描述/超时/间隔/四个开关），限高防
// 小窗溢出。
constexpr float kDialogFormHeight = 340.0F;

enum class ProfileGridItemKind { GroupHeader, Profile, Footer };

struct ProfileGridItem {
    ProfileGridItemKind kind = ProfileGridItemKind::Profile;
    std::string type;
    std::size_t profileIndex = 0;
    std::size_t profileCount = 0;
};

std::string profileTypeLabel(std::string_view type) {
    if (type == "remote") return "远程订阅";
    if (type == "local") return "本地文件";
    if (type == "pptp") return "PPTP 内网连接";
    if (type == "openvpn") return "OpenVPN 内网连接";
    return type.empty() ? "其他订阅" : "其他 · " + std::string(type);
}

bool isNativeVpnType(std::string_view type) {
    return type == "pptp" || type == "openvpn";
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

std::string pptpStateText(const store::PptpState& state) {
    if (!state.toolsAvailable) return "PPTP 引擎不可用";
    switch (state.state) {
    case vpn::ConnectionState::Connected:
        return std::format("已连接{}{}", state.interfaceName.empty()
                                              ? ""
                                              : " · " + state.interfaceName,
                           state.gateway.empty() ? ""
                                                 : " · 网关 " + state.gateway);
    case vpn::ConnectionState::Connecting: return "连接中…";
    case vpn::ConnectionState::Failed:
        return state.error.empty() ? "连接失败" : "连接失败：" + state.error;
    case vpn::ConnectionState::Disabled: return "已禁用";
    case vpn::ConnectionState::Idle: return "未连接";
    }
    return "未连接";
}

std::string openVpnStateText(const store::OpenVpnState& state) {
    if (!state.toolsAvailable) return "OpenVPN 引擎不可用";
    switch (state.state) {
    case vpn::ConnectionState::Connected:
        return std::format("已连接{}{}", state.interfaceName.empty()
                                              ? ""
                                              : " · " + state.interfaceName,
                           state.gateway.empty() ? ""
                                                 : " · 网关 " + state.gateway);
    case vpn::ConnectionState::Connecting: return "连接中…";
    case vpn::ConnectionState::Failed:
        return state.error.empty() ? "连接失败" : "连接失败：" + state.error;
    case vpn::ConnectionState::Disabled: return "已禁用";
    case vpn::ConnectionState::Idle: return "未连接";
    }
    return "未连接";
}

// 开关行：左标签（danger = error 色警示）+ 说明，右 Switch。Switch 卸载风险
// 不存在（原地改样式），OnChanged 内直接写 State 安全。
[[huxerui::composable]] huxerui::View ToggleRow(std::string label, std::string hint,
                                                bool danger,
                                                huxerui::State<bool> checked) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Column {
            huxerui::Text(std::move(label))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    danger ? theme.colors.error : theme.colors.on_surface}),
            huxerui::Text(std::move(hint))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }
            .With(huxerui::Spacing(2.0F))
            .With(huxerui::Grow(1.0F)),
        huxerui::Switch(checked.Get()).OnChanged(
            [checked](bool on) { checked = on; }),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 订阅选项表单（新建/编辑弹窗共用）：描述 + HTTP 超时/更新间隔 + 自动更新/
// 系统代理/内核代理/无效证书开关。字段值由调用方持有的 State 承载。
[[huxerui::composable]] huxerui::View ProfileOptionsForm(
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> autoUp, huxerui::State<bool> sysProxy,
    huxerui::State<bool> coreProxy, huxerui::State<bool> invalidCert) {
    return huxerui::Column {
        huxerui::TextField(desc.Get())
            .Label("描述（可选）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([desc](const huxerui::TextEditingValue& v) { desc = v; }),
        huxerui::Row {
            huxerui::TextField(timeout.Get())
                .Label("HTTP 超时（秒）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([timeout](const huxerui::TextEditingValue& v) {
                    timeout = v;
                })
                .With(huxerui::Grow(1.0F)),
            huxerui::TextField(interval.Get())
                .Label("更新间隔（分钟）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([interval](const huxerui::TextEditingValue& v) {
                    interval = v;
                })
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(8.0F)),
        ToggleRow("允许自动更新", "开启后按更新间隔自动拉新（间隔需 > 0）", false,
                  autoUp),
        PlatformControl(
            {PlatformCode::SystemProxy},
            [sysProxy] {
                return ToggleRow("使用系统代理更新", "经环境变量代理拉取订阅",
                                 false, sysProxy);
            }),
        ToggleRow("使用内核代理更新", "经本应用内核混合端口拉取（内核需运行）",
                  false, coreProxy),
        ToggleRow("允许无效证书（危险）", "跳过 HTTPS 证书校验，仅用于可信来源",
                  true, invalidCert),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// PPTP 原生订阅表单：只在订阅类型为 pptp 时显示。密码属于受控输入，
// 通过 PasswordField 统一提供显隐眼睛。
[[huxerui::composable]] huxerui::View PptpOptionsForm(
    huxerui::State<huxerui::TextEditingValue> server,
    huxerui::State<huxerui::TextEditingValue> username,
    huxerui::State<huxerui::TextEditingValue> password,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> routes,
    huxerui::State<bool> requireMppe) {
    return huxerui::Column {
        huxerui::TextField(server.Get())
            .Label("PPTP 服务器")
            .Placeholder("vpn.example.com")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([server](const huxerui::TextEditingValue& value) {
                server = value;
            }),
        huxerui::TextField(username.Get())
            .Label("用户名")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([username](const huxerui::TextEditingValue& value) {
                username = value;
            }),
        PasswordField(password),
        huxerui::Row {
            huxerui::TextField(timeout.Get())
                .Label("连接超时（秒）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([timeout](const huxerui::TextEditingValue& value) {
                    timeout = value;
                })
                .With(huxerui::Grow(1.0F)),
            huxerui::Switch(requireMppe.Get())
                .OnChanged([requireMppe](bool checked) { requireMppe = checked; }),
            huxerui::Text("要求 MPPE-128"),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::TextField(routes.Get())
            .Label("内网 CIDR（逗号或换行分隔，可为空）")
            .Placeholder("10.20.0.0/16, 10.30.0.0/16")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([routes](const huxerui::TextEditingValue& value) {
                routes = value;
            }),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// OpenVPN 使用原生 .ovpn 文本；支持 inline ca/cert/key/auth-user-pass，避免
// 页面另造一套只覆盖部分 CLI 参数的配置模型。route 只由统一订阅路由字段管理。
[[huxerui::composable]] huxerui::View OpenVpnOptionsForm(
    huxerui::State<huxerui::TextEditingValue> config,
    huxerui::State<huxerui::TextEditingValue> routes) {
    return huxerui::Column {
        huxerui::TextField(config.Get())
            .Label("OpenVPN 配置（.ovpn 文本，支持 inline 证书）")
            .Placeholder("client\\ndev tun\\nremote vpn.example.com 1194 udp")
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(8, 16))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([config](const huxerui::TextEditingValue& value) {
                config = value;
            }),
        huxerui::TextField(routes.Get())
            .Label("内网 CIDR（逗号或换行分隔，可为空）")
            .Placeholder("10.20.0.0/16, 10.30.0.0/16")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([routes](const huxerui::TextEditingValue& value) {
                routes = value;
            }),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// Android 端的编辑入口使用完整页面，避免弹窗被软键盘和窄视口挤压；桌面
// 仍由调用方使用 Dialog。表单 State 由 ProfilesPage 持有，页面与弹窗共用一份
// 数据和保存逻辑。
[[huxerui::composable]] huxerui::View ProfileEditPage(
    std::int64_t id, huxerui::State<huxerui::TextEditingValue> name,
    huxerui::State<huxerui::TextEditingValue> url,
    huxerui::State<std::string> type,
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> auto_update, huxerui::State<bool> system_proxy,
    huxerui::State<bool> core_proxy, huxerui::State<bool> invalid_cert,
    huxerui::State<huxerui::TextEditingValue> pptp_server,
    huxerui::State<huxerui::TextEditingValue> pptp_username,
    huxerui::State<huxerui::TextEditingValue> pptp_password,
    huxerui::State<huxerui::TextEditingValue> pptp_timeout,
    huxerui::State<huxerui::TextEditingValue> pptp_routes,
    huxerui::State<bool> pptp_mppe,
    huxerui::State<huxerui::TextEditingValue> openvpn_config,
    huxerui::State<huxerui::TextEditingValue> openvpn_routes,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::function<void()> on_back) {
    const auto save = [id, name, url, type, desc, timeout, interval, auto_update,
                       system_proxy, core_proxy, invalid_cert, pptp_server,
                       pptp_username, pptp_password, pptp_timeout, pptp_routes,
                       pptp_mppe, openvpn_config, openvpn_routes, tasks, toast,
                       on_back] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            db::Profile fields;
            fields.name = name.Get().text;
            fields.type = type.Get();
            fields.description = desc.Get().text;
            fields.timeoutSecs = parseNumber(timeout.Get(), 60);
            fields.intervalMins = parseNumber(interval.Get(), 0);
            fields.autoUpdate = auto_update.Get();
            fields.useSystemProxy = system_proxy.Get();
            fields.useCoreProxy = core_proxy.Get();
            fields.allowInvalidCert = invalid_cert.Get();
            if (fields.type == "pptp") {
                std::string configError;
                const auto config = makePptpConfig(
                    pptp_server.Get(), pptp_username.Get(), pptp_password.Get(),
                    pptp_timeout.Get(), pptp_mppe.Get(), configError);
                if (!config) {
                    toast.Show(configError);
                    co_return;
                }
                fields.nativeConfig = *config;
                fields.nativeRoutes = joinRoutes(parseRouteField(pptp_routes.Get().text));
                fields.url.clear();
            } else if (fields.type == "openvpn") {
                std::string configError;
                if (!openvpn::ParseOpenVpnConfig(openvpn_config.Get().text,
                                                 configError)) {
                    toast.Show(configError);
                    co_return;
                }
                fields.nativeConfig = openvpn_config.Get().text;
                fields.nativeRoutes = joinRoutes(
                    parseRouteField(openvpn_routes.Get().text));
                fields.url.clear();
            } else {
                fields.url = url.Get().text;
            }
            const std::string err = co_await RunOnTaskThread(
                [id, fields] {
                    auto& ps = store::profilesStore();
                    if (!ps.updateProfile(id, fields)) return ps.lastError();
                    return std::string{};
                });
            if (err.empty()) {
                toast.Show("已保存");
                on_back();
            } else {
                toast.Show(err);
            }
        });
    };

    return PageScaffold(
        "编辑订阅", huxerui::Button("返回").OnClick(on_back),
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    huxerui::Text(type.Get() == "pptp"
                                      ? "类型：PPTP 内网连接"
                                      : type.Get() == "openvpn"
                                      ? "类型：OpenVPN 内网连接"
                                      : type.Get() == "local" ? "类型：本地文件"
                                                               : "类型：远程订阅"),
                    huxerui::TextField(name.Get())
                        .Label("名称")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([name](const huxerui::TextEditingValue& value) {
                            name = value;
                        }),
                    type.Get() == "pptp"
                        ? huxerui::View{PptpOptionsForm(
                              pptp_server, pptp_username, pptp_password,
                              pptp_timeout, pptp_routes, pptp_mppe)}
                        : type.Get() == "openvpn"
                        ? huxerui::View{OpenVpnOptionsForm(openvpn_config,
                                                           openvpn_routes)}
                        : huxerui::View{huxerui::Column {
                              huxerui::TextField(url.Get())
                                  .Label("订阅链接（留空 = 本地导入）")
                                  .Placeholder("https://...")
                                  .Variant(huxerui::TextFieldVariant::Outlined)
                                  .OnChanged([url](
                                                 const huxerui::TextEditingValue& value) {
                                      url = value;
                                  }),
                              ProfileOptionsForm(desc, timeout, interval,
                                                 auto_update, system_proxy,
                                                 core_proxy, invalid_cert),
                          }},
                    type.Get() == "pptp" || type.Get() == "openvpn"
                        ? huxerui::View{huxerui::TextField(desc.Get())
                                            .Label("描述（可选）")
                                            .Variant(huxerui::TextFieldVariant::Outlined)
                                            .OnChanged([desc](
                                                           const huxerui::TextEditingValue& value) {
                                                desc = value;
                                            })}
                        : huxerui::View{huxerui::Row{}},
                    huxerui::Row {
                        huxerui::Button("取消").OnClick(on_back),
                        huxerui::Button("保存").OnClick(save),
                    }.With(huxerui::Spacing(8.0F),
                           huxerui::MainAlign(
                               huxerui::MainAxisAlignment::End)),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch))),
                CompactFloatingNavigationFooter(),
            }
                .With(huxerui::Spacing(12.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch))
                .With(huxerui::Grow(1.0F))));
}

// 单张订阅卡：纯视图（零弹窗 State；菜单句柄/任务域由页面下发），弹窗经
// openXxx(id) 回调到页面级懒加载打开。
[[huxerui::composable]] huxerui::View ProfileCard(
    const db::Profile& profile, bool compact, huxerui::MenuHandle menu,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::function<void()> reload,
    const store::PptpState& pptpState,
    const store::OpenVpnState& openVpnState, bool connectionSelected,
    const std::function<void(std::int64_t, bool)>& toggleConnection,
    const std::function<void(std::int64_t)>& openEditInfo,
    const std::function<void(std::int64_t)>& openEditRules,
    const std::function<void(std::int64_t)>& openEditFile,
    const std::function<void(std::int64_t)>& openQr) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const std::int64_t id = profile.id;
    const bool nativeVpn = isNativeVpnType(profile.type);

    auto action = [tasks, toast, reload](std::function<std::string()> job) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const std::string err = co_await RunOnTaskThread(std::move(job));
            if (!err.empty()) toast.Show(err);
            reload();
        });
    };

    // 右上角刷新图标：裸 Image + Tint 着色（IconButton 不着色矢量资源；
    // 同 apitab 标题栏齿轮配方），自绘 24pt 正方形热区。
    huxerui::View refreshButton = huxerui::Row{};
    if (!nativeVpn) {
        refreshButton = huxerui::Row {
            huxerui::Image(app::images::refresh)
                .Fit(huxerui::ImageFit::Contain)
                .Align(huxerui::HorizontalAlignment::Center,
                       huxerui::VerticalAlignment::Center)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
        }
            .With(huxerui::Padding(5.0F),
                  huxerui::CornerRadius(islands.nested_radius),
                  huxerui::Tooltip("更新订阅"),
                  huxerui::Focusable(true),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "更新订阅"})
            .OnClick([action, id] {
                action([id]() -> std::string {
                    auto& ps = store::profilesStore();
                    if (!ps.refresh(id)) return ps.lastError();
                    return "";
                });
            });
    }

    const std::string primaryLine = [&] {
        if (!nativeVpn) {
            return profile.url.empty() ? std::string("本地导入") : profile.url;
        }
        if (profile.type == "openvpn") return std::string("OpenVPN · CLI 配置");
        std::string parseError;
        const auto config = pptp::ParsePptpConfig(profile.nativeConfig, parseError);
        return config ? "PPTP · " + config->server
                      : std::string("PPTP 配置无效");
    }();

    huxerui::View card = Card(huxerui::Column {
        huxerui::Row {
            huxerui::Text(truncateOneLine(profile.name, 16)).Style(
                huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    theme.colors.on_surface}),
            profile.selected
                ? huxerui::View{
                      huxerui::Text("使用中").Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_primary})}
                      .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                8.0F, 2.0F)),
                            huxerui::Background(theme.colors.primary),
                            huxerui::CornerRadius(islands.nested_radius))
                : huxerui::View{huxerui::Row{}},
            nativeVpn
                ? huxerui::View{huxerui::Checkbox(connectionSelected)
                                    .OnChanged([toggleConnection, tasks,
                                                id](bool checked) {
                                        // Checkbox 的 OnChanged 仍处于指针释放事件
                                        // 路径；延迟一拍再改 StateList，避免 VirtualGrid
                                        // 重组导致 HuxerUI 同一事件的 pointer session
                                        // 迭代器失效。
                                        tasks.Launch([toggleConnection, id,
                                                      checked]()
                                                         -> huxerui::Task<void> {
                                            co_await huxerui::Delay(
                                                std::chrono::duration<double>{0});
                                            toggleConnection(id, checked);
                                        });
                                    })}
                : huxerui::View{huxerui::Row{}},
            huxerui::Spacer(),
            std::move(refreshButton),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(truncateOneLine(primaryLine, 40))
            .Style(huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip),
                                      theme.colors.on_surface_variant}),
        huxerui::Text(truncateOneLine(
                          nativeVpn
                              ? (profile.nativeRoutes.empty()
                                     ? "未设置内网路由"
                                     : "路由：" + profile.nativeRoutes)
                              : (profile.description.empty() ? "—"
                                                              : profile.description),
                          44))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                nativeVpn || profile.description.empty()
                    ? theme.colors.outline
                    : theme.colors.on_surface_variant}),
        huxerui::Row {
            huxerui::Text(nativeVpn
                              ? profile.type == "openvpn" ? "OpenVPN" : "PPTP"
                              : profile.type == "local" ? "本地" : "远程")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            !nativeVpn && profile.autoUpdate && profile.intervalMins > 0
                ? huxerui::View{huxerui::Text(std::format("自动 {} 分钟",
                                                          profile.intervalMins))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(
                                            font_size::kCaption),
                                        theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
            huxerui::Spacer(),
            nativeVpn
                ? huxerui::View{huxerui::Text(profile.type == "openvpn"
                                                   ? openVpnStateText(openVpnState)
                                                   : pptpStateText(pptpState))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(font_size::kCaption),
                                        (profile.type == "openvpn"
                                             ? openVpnState.state
                                             : pptpState.state) ==
                                                vpn::ConnectionState::Failed
                                            ? theme.colors.error
                                            : theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
            huxerui::Text(truncateOneLine(
                              nativeVpn
                                  ? ""
                                  : profile.error.empty()
                                  ? (profile.updatedAt > 0
                                         ? "更新于 " + formatTime(profile.updatedAt)
                                         : "未拉取")
                                  : "错误：" + profile.error,
                              44))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    nativeVpn || profile.error.empty()
                        ? theme.colors.on_surface_variant
                        : theme.colors.error}),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        // 流量行 + 进度条（订阅响应头 subscription-userinfo；total 未知不显示）。
        !nativeVpn && profile.totalBytes > 0
            ? huxerui::View{huxerui::Column {
                  huxerui::Row {
                      huxerui::Text("已用 " + formatBytes(profile.usedBytes) +
                                    " / " + formatBytes(profile.totalBytes))
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_surface_variant}),
                      huxerui::Spacer(),
                      huxerui::Text(std::format(
                          "{}%", static_cast<int>(
                                     std::clamp(
                                         static_cast<double>(profile.usedBytes) /
                                             static_cast<double>(
                                                 profile.totalBytes),
                                         0.0, 1.0) *
                                     100.0)))
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_surface_variant}),
                  }
                      .With(huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Center)),
                  huxerui::ProgressBar(std::clamp(
                                           static_cast<float>(
                                               profile.usedBytes) /
                                               static_cast<float>(
                                                   profile.totalBytes),
                                           0.0F, 1.0F))
                      .With(huxerui::Frame{.height = 4.0F}),
              }
                                .With(huxerui::Spacing(4.0F),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Stretch))}
            : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));

    // 矩形卡统一尺寸：定宽定高（内容已单行截断，ClipChildren 兜底），
    // Compact 由父列 Stretch 整宽（高度仍统一）。
    if (!compact) {
        card = std::move(card).With(huxerui::Frame{.width = kCardWidth,
                                                   .height = kCardHeight},
                                    huxerui::ClipChildren());
    } else {
        card = std::move(card).With(huxerui::Frame{.height = kCardHeight},
                                    huxerui::ClipChildren());
    }
    // 选中（使用中）状态：primary 描边，与「使用中」徽标呼应。
    if (profile.selected) {
        card = std::move(card).With(
            huxerui::Border(theme.colors.primary, 2.0F));
    }
    return std::move(card)
        // 远程/本地 mihomo 订阅暂时保持单选：点击哪张卡片，哪张就是当前订阅。
        // 原生 PPTP/OpenVPN 仍由复选框进入多连接流程，后续再统一抽象。
        .OnClick([action, id, selected = profile.selected, nativeVpn] {
                if (selected || nativeVpn) return;
                action([id]() -> std::string {
                    auto& ps = store::profilesStore();
                    if (!ps.activate(id)) return ps.lastError();
                    return "";
                });
            })
        // 右键上下文菜单（跟随点击位置弹出）。
        .On<huxerui::ViewEvents::ContextMenuRequested>(
            [menu, tasks, action, openEditInfo, openEditRules, openEditFile, openQr,
             id, homepage = profile.homepage, url = profile.url,
             selected = profile.selected, nativeVpn,
             openVpn = profile.type == "openvpn"](
                huxerui::Point pos) {
                std::vector<huxerui::MenuEntry> entries;
                if (!selected && !nativeVpn) {
                    entries.push_back(huxerui::MenuItem("使用", [action, id] {
                        action([id]() -> std::string {
                            auto& ps = store::profilesStore();
                            if (!ps.activate(id)) return ps.lastError();
                            return "";
                        });
                    }));
                }
                if (!nativeVpn) {
                    entries.push_back(huxerui::MenuItem("更新", [action, id] {
                        action([id]() -> std::string {
                            auto& ps = store::profilesStore();
                            if (!ps.refresh(id)) return ps.lastError();
                            return "";
                        });
                    }));
                }
                if (!nativeVpn && !homepage.empty()) {
                    entries.push_back(huxerui::MenuItem("首页",
                                                        [action, homepage] {
                        action([homepage]() -> std::string {
                            core::openInBrowser(homepage);
                            return "";
                        });
                    }));
                }
                if (!nativeVpn && !url.empty()) {
                    entries.push_back(huxerui::MenuItem("分享二维码",
                                                        [openQr, id] {
                        openQr(id);
                    }));
                }
                // 原生 VPN 卡片没有“使用/更新”等前置菜单项，不能在菜单开头
                // 插入分隔线；HuxerUI 要求 MenuSection 必须夹在两个菜单项之间。
                if (!entries.empty()) entries.push_back(huxerui::MenuSection{});
                entries.push_back(huxerui::MenuItem("编辑信息", [openEditInfo, id] {
                    openEditInfo(id);
                }));
                if (!nativeVpn) {
                    entries.push_back(huxerui::MenuItem("编辑规则",
                                                        [openEditRules, id] {
                        openEditRules(id);
                    }));
                    entries.push_back(huxerui::MenuItem("编辑文件",
                                                        [openEditFile, id] {
                        openEditFile(id);
                    }));
                }
                entries.push_back(huxerui::MenuSection{});
                entries.push_back(huxerui::MenuItem("删除", [action, id, nativeVpn,
                                                               openVpn] {
                    action([id, nativeVpn, openVpn]() -> std::string {
                        if (nativeVpn) {
                            if (openVpn) store::vpnStore().forgetOpenVpn(id);
                            else store::vpnStore().forgetPptp(id);
                        }
                        auto& ps = store::profilesStore();
                        ps.remove(id);
                        return ps.lastError();
                    });
                }));
                // ContextMenuRequested 仍处于鼠标释放事件路径；同步创建菜单
                // 会重组浮层树，使 HuxerUI 正在清理的 pointer session 迭代器
                // 失效。延迟到下一帧再挂载菜单。
                tasks.Launch([menu, pos, entries = std::move(entries)]()
                                 mutable -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    menu.ShowAt(pos, std::move(entries));
                });
            })
        .Key(id);
}

} // namespace

[[huxerui::composable]] huxerui::View ProfilesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto menu = huxerui::UseMenu();
    auto clipboard = application.Clipboard();
    auto profiles = huxerui::UseStateList<db::Profile>();
    auto pptpStates = huxerui::UseStateList<store::PptpState>();
    auto openVpnStates = huxerui::UseStateList<store::OpenVpnState>();
    auto connectionSelection = huxerui::UseStateList<std::int64_t>();

    // ---- 新建订阅弹窗 ----
    auto newName = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newUrl = huxerui::UseState(huxerui::TextEditingValue{""});
    auto importing = huxerui::UseState(false);
    auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto newTypeIdx = huxerui::UseState<std::size_t>(0);
    auto newDesc = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newTimeout = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newInterval = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newAuto = huxerui::UseState(false);
    auto newSys = huxerui::UseState(false);
    auto newCore = huxerui::UseState(false);
    auto newCert = huxerui::UseState(false);
    auto newPptpServer = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newPptpUsername = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newPptpPassword = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newPptpTimeout = huxerui::UseState(huxerui::TextEditingValue{"30"});
    auto newPptpRoutes = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newPptpMppe = huxerui::UseState(true);
    auto newOpenVpnConfig = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newOpenVpnRoutes = huxerui::UseState(huxerui::TextEditingValue{""});
    auto pickedPath = huxerui::UseState<std::string>("");

    // ---- 编辑订阅弹窗（页面级一份；卡片按 id 打开，避免 N 卡 × N 状态）----
    auto editName = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editUrl = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editType = huxerui::UseState<std::string>("remote");
    auto editDesc = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editTimeout = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editInterval = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editAuto = huxerui::UseState(false);
    auto editSys = huxerui::UseState(false);
    auto editCore = huxerui::UseState(false);
    auto editCert = huxerui::UseState(false);
    auto editPptpServer = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editPptpUsername = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editPptpPassword = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editPptpTimeout = huxerui::UseState(huxerui::TextEditingValue{"30"});
    auto editPptpRoutes = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editPptpMppe = huxerui::UseState(true);
    auto editOpenVpnConfig = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editOpenVpnRoutes = huxerui::UseState(huxerui::TextEditingValue{""});

    // ---- 编辑规则弹窗（工作 YAML + 规则行 + 输入行 + 脏标记）----
    auto editYamlText = huxerui::UseState<std::string>("");
    auto editRules = huxerui::UseStateList<std::string>();
    auto editRuleInput = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editRulesDirty = huxerui::UseState(false);

    // ---- 编辑文件弹窗（懒加载 + 非受控大文本：shared_ptr 原地写，逐键零重组）----
    auto fileContent = huxerui::UseState(std::make_shared<std::string>());
    auto fileLoading = huxerui::UseState(false);
    // Android 编辑订阅时切换到页面；0 表示仍在订阅列表。
    auto editPageId = huxerui::UseState<std::int64_t>(0);

    // 列表泵：2s 一拍重读（State 相等时短路，无重组；CRUD 后手动 reload）。
    huxerui::Lifecycle(
        [tasks, profiles] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{2.0}, [=] {
                    ReplaceStateList(profiles, store::profilesStore().list());
                    return true;
                });
            });
            return [] {};
        },
        0);

    huxerui::Lifecycle(
        [tasks, pptpStates, openVpnStates] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await RunOnTaskThread([] { store::vpnStore().init(); });
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    ReplaceStateList(pptpStates, store::vpnStore().states());
                    ReplaceStateList(openVpnStates, store::vpnStore().openVpnStates());
                    return true;
                });
            });
            return [] {};
        },
        0);

    auto reload = [tasks] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            // 列表由 2s 泵刷新；这里仅给一拍推迟，保证任务内 State 写不与
            // 点击路径叠加。
        });
    };

    auto findProfile = [profiles](std::int64_t id) -> std::optional<db::Profile> {
        for (const auto& p : profiles) {
            if (p.id == id) return p;
        }
        return std::nullopt;
    };

    auto isConnectionSelected = [connectionSelection](std::int64_t id) {
        for (const std::int64_t selected : connectionSelection) {
            if (selected == id) return true;
        }
        return false;
    };

    auto toggleConnection = [connectionSelection](std::int64_t id, bool checked) {
        for (std::size_t index = 0; index < connectionSelection.Size(); ++index) {
            if (connectionSelection[index] != id) continue;
            if (!checked) connectionSelection.Erase(index);
            return;
        }
        if (checked) connectionSelection.PushBack(id);
    };

    auto openSelectedConnections = [tasks, toast, connectionSelection] {
        std::vector<std::int64_t> ids;
        for (const std::int64_t id : connectionSelection) ids.push_back(id);
        if (ids.empty()) {
            toast.Show("请先选择 PPTP 或 OpenVPN 订阅");
            return;
        }
        tasks.Launch([ids, toast, connectionSelection]() -> huxerui::Task<void> {
            const auto result = co_await RunOnTaskThread([ids] {
                int opened = 0;
                int failed = 0;
                std::string firstError;
                const auto all = store::profilesStore().list();
                for (const std::int64_t id : ids) {
                    const auto it = std::ranges::find_if(
                        all, [id](const db::Profile& profile) { return profile.id == id; });
                    if (it == all.end() || !isNativeVpnType(it->type)) continue;
                    std::string error;
                    const bool connected = it->type == "openvpn"
                                               ? store::vpnStore().connectOpenVpn(*it, error)
                                               : store::vpnStore().connectPptp(*it, error);
                    if (connected) {
                        ++opened;
                    } else {
                        ++failed;
                        if (firstError.empty()) firstError = error;
                    }
                }
                return std::tuple{opened, failed, firstError};
            });
            connectionSelection.Clear();
            if (std::get<1>(result) == 0) {
                toast.Show(std::format("已打开 {} 个连接", std::get<0>(result)));
            } else {
                toast.Show(std::format("已打开 {} 个，失败 {} 个{}", std::get<0>(result),
                                      std::get<1>(result),
                                      std::get<2>(result).empty()
                                          ? ""
                                          : "：" + std::get<2>(result)));
            }
        });
    };

    auto closeSelectedConnections = [tasks, toast, connectionSelection] {
        std::vector<std::int64_t> ids;
        for (const std::int64_t id : connectionSelection) ids.push_back(id);
        if (ids.empty()) {
            toast.Show("请先选择 PPTP 或 OpenVPN 订阅");
            return;
        }
        tasks.Launch([ids, toast, connectionSelection]() -> huxerui::Task<void> {
            co_await RunOnTaskThread([ids] {
                const auto all = store::profilesStore().list();
                for (const std::int64_t id : ids) {
                    const auto it = std::ranges::find_if(
                        all, [id](const db::Profile& profile) { return profile.id == id; });
                    if (it == all.end()) continue;
                    if (it->type == "openvpn") store::vpnStore().disconnectOpenVpn(id);
                    else if (it->type == "pptp") store::vpnStore().disconnectPptp(id);
                }
            });
            connectionSelection.Clear();
            toast.Show("已关闭选中的连接");
        });
    };

    // ---- 编辑信息（按订阅类型显示对应表单；仅落库，下次更新/连接生效）----
    auto loadEditInfo = [tasks, findProfile, editName, editUrl, editType, editDesc,
                         editTimeout, editInterval, editAuto, editSys, editCore,
                         editCert, editPptpServer, editPptpUsername,
                         editPptpPassword, editPptpTimeout, editPptpRoutes,
                         editPptpMppe, editOpenVpnConfig, editOpenVpnRoutes](
        std::int64_t id, std::function<void()> next) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile) co_return;
            editName = huxerui::TextEditingValue{profile->name};
            editUrl = huxerui::TextEditingValue{profile->url};
            editType = profile->type;
            editDesc = huxerui::TextEditingValue{profile->description};
            editTimeout =
                huxerui::TextEditingValue{std::format("{}", profile->timeoutSecs)};
            editInterval =
                huxerui::TextEditingValue{std::format("{}", profile->intervalMins)};
            editAuto = profile->autoUpdate;
            editSys = profile->useSystemProxy;
            editCore = profile->useCoreProxy;
            editCert = profile->allowInvalidCert;
            std::string pptpError;
            const auto config = pptp::ParsePptpConfig(profile->nativeConfig, pptpError);
            editPptpServer = huxerui::TextEditingValue{
                config ? config->server : ""};
            editPptpUsername = huxerui::TextEditingValue{
                config ? config->username : ""};
            editPptpPassword = huxerui::TextEditingValue{
                config ? config->password : ""};
            editPptpTimeout = huxerui::TextEditingValue{
                config ? std::to_string(config->connectTimeoutSecs) : "30"};
            editPptpRoutes = huxerui::TextEditingValue{profile->nativeRoutes};
            editPptpMppe = config ? config->requireMppe : true;
            editOpenVpnConfig = huxerui::TextEditingValue{profile->nativeConfig};
            editOpenVpnRoutes = huxerui::TextEditingValue{profile->nativeRoutes};
            next();
        });
    };

    auto showEditInfo = [loadEditInfo, dialog, tasks, toast, editName, editUrl,
                         editType, editDesc, editTimeout, editInterval, editAuto,
                         editSys, editCore, editCert, editPptpServer,
                         editPptpUsername, editPptpPassword, editPptpTimeout,
                         editPptpRoutes, editPptpMppe, editOpenVpnConfig,
                         editOpenVpnRoutes,
                         textColor = theme.colors.on_surface,
                         hintColor = theme.colors.on_surface_variant](
                            std::int64_t id) {
        loadEditInfo(id, [dialog, tasks, toast, editName, editUrl, editType,
                          editDesc, editTimeout, editInterval, editAuto, editSys,
                          editCore, editCert, editPptpServer, editPptpUsername,
                          editPptpPassword, editPptpTimeout, editPptpRoutes,
                          editPptpMppe, editOpenVpnConfig, editOpenVpnRoutes,
                          id, textColor, hintColor] {
            dialog.Show(
                [tasks, toast, editName, editUrl, editType, editDesc, editTimeout,
                 editInterval, editAuto, editSys, editCore, editCert,
                 editPptpServer, editPptpUsername, editPptpPassword,
                 editPptpTimeout, editPptpRoutes, editPptpMppe, editOpenVpnConfig,
                 editOpenVpnRoutes, id,
                 textColor, hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                    const bool pptp = editType.Get() == "pptp";
                    const bool openvpn = editType.Get() == "openvpn";
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑订阅", huxerui::TextRole::Title),
                        huxerui::ScrollView(huxerui::Column {
                            huxerui::Text(pptp ? "类型：PPTP 内网连接"
                                               : openvpn ? "类型：OpenVPN 内网连接"
                                               : editType.Get() == "local"
                                                     ? "类型：本地文件"
                                                     : "类型：远程订阅"),
                            huxerui::TextField(editName.Get())
                                .Label("名称")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editName](const huxerui::TextEditingValue& v) {
                                        editName = v;
                                    }),
                            pptp
                                ? huxerui::View{PptpOptionsForm(
                                      editPptpServer, editPptpUsername,
                                      editPptpPassword, editPptpTimeout,
                                      editPptpRoutes, editPptpMppe)}
                                : openvpn
                                ? huxerui::View{OpenVpnOptionsForm(
                                      editOpenVpnConfig, editOpenVpnRoutes)}
                                : huxerui::View{huxerui::Column {
                                      huxerui::TextField(editUrl.Get())
                                          .Label("订阅链接（留空 = 本地导入）")
                                          .Placeholder("https://...")
                                          .Variant(huxerui::TextFieldVariant::Outlined)
                                          .OnChanged([editUrl](
                                                         const huxerui::TextEditingValue& v) {
                                              editUrl = v;
                                          }),
                                      ProfileOptionsForm(
                                          editDesc, editTimeout, editInterval,
                                          editAuto, editSys, editCore, editCert),
                                  }},
                            pptp || openvpn
                                ? huxerui::View{huxerui::TextField(editDesc.Get())
                                                    .Label("描述（可选）")
                                                    .Variant(huxerui::TextFieldVariant::Outlined)
                                                    .OnChanged([editDesc](
                                                                   const huxerui::TextEditingValue& v) {
                                                        editDesc = v;
                                                    })}
                                : huxerui::View{huxerui::Row{}},
                        }
                                           .With(huxerui::Spacing(12.0F),
                                                 huxerui::CrossAlign(
                                                     huxerui::CrossAxisAlignment::
                                                         Stretch)))
                            .With(huxerui::Frame{.height = kDialogFormHeight}),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                std::string configError;
                                std::optional<std::string> nativeConfig;
                                if (editType.Get() == "pptp") {
                                    nativeConfig = makePptpConfig(
                                        editPptpServer.Get(), editPptpUsername.Get(),
                                        editPptpPassword.Get(), editPptpTimeout.Get(),
                                        editPptpMppe.Get(), configError);
                                    if (!nativeConfig) {
                                        toast.Show(configError);
                                        return;
                                    }
                                } else if (editType.Get() == "openvpn") {
                                    if (!openvpn::ParseOpenVpnConfig(
                                            editOpenVpnConfig.Get().text, configError)) {
                                        toast.Show(configError);
                                        return;
                                    }
                                    nativeConfig = editOpenVpnConfig.Get().text;
                                }
                                ctx.Dismiss();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    db::Profile fields;
                                    fields.name = editName.Get().text;
                                    fields.type = editType.Get();
                                    fields.url = isNativeVpnType(fields.type)
                                                     ? ""
                                                     : editUrl.Get().text;
                                    fields.description = editDesc.Get().text;
                                    fields.timeoutSecs =
                                        parseNumber(editTimeout.Get(), 60);
                                    fields.intervalMins =
                                        parseNumber(editInterval.Get(), 0);
                                    fields.autoUpdate = editAuto.Get();
                                    fields.useSystemProxy = editSys.Get();
                                    fields.useCoreProxy = editCore.Get();
                                    fields.allowInvalidCert = editCert.Get();
                                    if (nativeConfig) {
                                        fields.nativeConfig = *nativeConfig;
                                        fields.nativeRoutes = joinRoutes(
                                            parseRouteField(fields.type == "openvpn"
                                                                ? editOpenVpnRoutes.Get().text
                                                                : editPptpRoutes.Get().text));
                                    }
                                    const std::string err = co_await RunOnTaskThread(
                                        [id, fields] {
                                            auto& ps = store::profilesStore();
                                            if (!ps.updateProfile(id, fields))
                                                return ps.lastError();
                                            return std::string{};
                                        });
                                    toast.Show(err.empty() ? "已保存" : err);
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 420.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    const PlatformCodeList platformCodes = ResolvePlatformCodes(
        ResolvePlatformInfo(huxerui::UseViewportClass()), false);
    const bool isAndroid = HasPlatformCode(platformCodes, PlatformCode::Android);
    const bool pptpSupported =
        HasPlatformCode(platformCodes, PlatformCode::PptpEngine);
    const bool openvpnSupported =
        HasPlatformCode(platformCodes, PlatformCode::OpenVpnEngine);
    auto openEditInfo = [isAndroid, loadEditInfo, showEditInfo, editPageId](
                            std::int64_t id) {
        if (!isAndroid) {
            showEditInfo(id);
            return;
        }
        loadEditInfo(id, [editPageId, id] { editPageId = id; });
    };

    // ---- 编辑规则（rules 列表 + 前置/后置；工作副本，保存才落盘）----
    auto showEditRules = [dialog, tasks, toast, editYamlText, editRules,
                          editRuleInput, editRulesDirty,
                          textColor = theme.colors.on_surface,
                          hintColor = theme.colors.on_surface_variant](
                             std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const std::string yaml =
                co_await RunOnTaskThread([id] {
                    return store::profilesStore().yamlOf(id);
                });
            editYamlText = yaml;
            ReplaceStateList(editRules, store::parseRules(yaml));
            editRuleInput = huxerui::TextEditingValue{""};
            editRulesDirty = false;
            dialog.Show(
                [tasks, toast, editYamlText, editRules, editRuleInput,
                 editRulesDirty, id, textColor, hintColor](
                    huxerui::DialogContext ctx) -> huxerui::View {
                    // 规则行（动态列表：键用行号；行内无状态）。
                    huxerui::View ruleList;
                    if (editRules.Empty()) {
                        ruleList = huxerui::Column {
                            huxerui::Text("订阅没有 rules 规则")
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    hintColor}),
                        }.With(huxerui::Padding(12.0F),
                               huxerui::Frame{.height = 300.0F});
                    } else {
                        ruleList = huxerui::VirtualList(
                                       editRules.Size(),
                                       [editRules, textColor,
                                        hintColor](std::size_t index) {
                                           return huxerui::Row {
                                               huxerui::Text(std::format("{}", index + 1))
                                                   .Style(huxerui::TextStyle{
                                                       huxerui::Font::System(
                                                           font_size::kCaption),
                                                       hintColor})
                                                   .With(huxerui::Frame{.width = 32.0F}),
                                               huxerui::Text(truncateOneLine(
                                                                 editRules[index], 72))
                                                   .Style(huxerui::TextStyle{
                                                       huxerui::Font::Monospace(
                                                           font_size::kMonoBody),
                                                       textColor}),
                                           }
                                               .With(
                                                   huxerui::Spacing(6.0F),
                                                   huxerui::CrossAlign(
                                                       huxerui::CrossAxisAlignment::Center))
                                               .Key(std::to_string(index));
                                       })
                                       .ItemExtent(28.0F)
                                       .With(huxerui::Frame{.height = 300.0F},
                                             huxerui::ScrollBar());
                    }
                    auto addRule = [=](bool prepend) {
                        const std::string text = editRuleInput.Get().text;
                        if (text.empty()) return;
                        const std::string yaml = store::insertRule(
                            editYamlText.Get(), text, prepend);
                        editYamlText = yaml;
                        ReplaceStateList(editRules, store::parseRules(yaml));
                        editRuleInput = huxerui::TextEditingValue{""};
                        editRulesDirty = true;
                    };
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑规则", huxerui::TextRole::Title),
                        huxerui::Text("前置插到列表头、后置追加到尾；点「保存」"
                                      "写回订阅文件，启用中的订阅将重启内核生效。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                hintColor}),
                        std::move(ruleList),
                        huxerui::Row {
                            huxerui::TextField(editRuleInput.Get())
                                .Label("规则，如 DOMAIN-SUFFIX,example.com,代理组")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editRuleInput](
                                        const huxerui::TextEditingValue& v) {
                                        editRuleInput = v;
                                    })
                                .With(huxerui::Grow(1.0F)),
                            huxerui::Button("前置").OnClick([=] { addRule(true); }),
                            huxerui::Button("后置").OnClick([=] { addRule(false); }),
                        }
                            .With(huxerui::Spacing(8.0F),
                                  huxerui::CrossAlign(
                                      huxerui::CrossAxisAlignment::Center)),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                ctx.Dismiss();
                                if (!editRulesDirty.Get()) return;
                                const std::string content = editYamlText.Get();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const std::string err = co_await
                                        RunOnTaskThread([id, content] {
                                            auto& ps = store::profilesStore();
                                            if (!ps.saveYaml(id, content))
                                                return ps.lastError();
                                            return std::string{};
                                        });
                                    toast.Show(err.empty() ? "规则已保存" : err);
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 620.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    // ---- 编辑文件（懒加载 YAML + 非受控多行编辑：OnChanged 只写 shared_ptr
    // 指向的内容，不触发 State 写 → 无逐键全文重排版；保存取现值）----
    auto showEditFile = [dialog, tasks, toast, fileContent, fileLoading,
                         hintColor = theme.colors.on_surface_variant](
                            std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            *fileContent.Get() = "";
            fileLoading = true;
            dialog.Show(
                [tasks, toast, fileContent, fileLoading, id, hintColor](
                    huxerui::DialogContext ctx) -> huxerui::View {
                    const huxerui::View body =
                        fileLoading.Get()
                            ? huxerui::View{
                                  huxerui::Column {
                                      huxerui::ProgressCircle()
                                          .With(huxerui::Frame{
                                              .width = 28.0F,
                                              .height = 28.0F}),
                                      huxerui::Text("正在加载订阅文件…")
                                          .Style(huxerui::TextStyle{
                                              huxerui::Font::System(
                                                  font_size::kCaption),
                                              hintColor}),
                                  }
                                      .With(huxerui::Spacing(10.0F),
                                            huxerui::Padding(24.0F),
                                            huxerui::MainAlign(
                                                huxerui::MainAxisAlignment::
                                                    Center),
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Center))}
                              : huxerui::View{
                                    huxerui::TextField(huxerui::
                                                           TextEditingValue{
                                                               *fileContent
                                                                    .Get()})
                                        .Variant(
                                            huxerui::TextFieldVariant::Outlined)
                                        .LineLimits(huxerui::
                                                        TextFieldLineLimits::
                                                            MultiLine(14, 22))
                                        // 非受控：只落 shared_ptr，不回写
                                        // State（避免逐键全文重组）。
                                        .OnChanged(
                                            [fileContent](
                                                const huxerui::TextEditingValue&
                                                    v) {
                                                *fileContent.Get() = v.text;
                                            })};
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑文件", huxerui::TextRole::Title),
                        huxerui::Text("直接编辑订阅 YAML 原文；保存后若该订阅"
                                      "启用中，内核将重启使改动生效。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                hintColor}),
                        body,
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                ctx.Dismiss();
                                if (fileLoading.Get()) return;
                                const std::string content = *fileContent.Get();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const std::string err = co_await
                                        RunOnTaskThread([id, content] {
                                            auto& ps = store::profilesStore();
                                            if (!ps.saveYaml(id, content))
                                                return ps.lastError();
                                            return std::string{};
                                        });
                                    toast.Show(err.empty() ? "已保存" : err);
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 640.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
            // 文件读取在任务线程；完成后经 fileLoading=false 触发重组展示。
            const std::string yaml =
                co_await RunOnTaskThread([id] {
                    return store::profilesStore().yamlOf(id);
                });
            *fileContent.Get() = yaml;
            fileLoading = false;
        });
    };

    // ---- 分享二维码 ----
    auto showQr = [dialog, tasks, toast, profiles, findProfile, clipboard,
                   hintColor = theme.colors.on_surface_variant](std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile || profile->url.empty()) co_return;
            const std::string url = profile->url;
            dialog.Show(
                [url, clipboard, toast,
                 hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("分享订阅", huxerui::TextRole::Title),
                        huxerui::Text(truncateOneLine(url, 60))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::Monospace(font_size::kChip),
                                hintColor}),
                        huxerui::Canvas(QrPainter(url))
                            .With(huxerui::Frame{.width = 240.0F,
                                                 .height = 240.0F}),
                        huxerui::Row {
                            huxerui::Button("复制链接").OnClick(
                                [clipboard, toast, url] {
                                    if (clipboard->WriteText(url)) {
                                        toast.Show("已复制到剪贴板");
                                    } else {
                                        toast.Show("复制失败");
                                    }
                                }),
                            huxerui::Button("关闭").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::End),
                               huxerui::Spacing(8.0F)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 320.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    // ---- 新建订阅弹窗 ----
    // 类型（远程/本地/PPTP/OpenVPN）+ 名称 + 类型表单；远程导入是网络下载（阻塞），走任务
    // 线程，成功才关弹窗。打开前清空上一轮的输入。
    auto showCreateDialog = [dialog, tasks, toast, picker, newName, newUrl,
                             newTypeIdx, newDesc, newTimeout, newInterval,
                             newAuto, newSys, newCore, newCert, newPptpServer,
                             newPptpUsername, newPptpPassword, newPptpTimeout,
                             newPptpRoutes, newPptpMppe, newOpenVpnConfig,
                             newOpenVpnRoutes, pickedPath, importing,
                             pptpSupported, openvpnSupported,
                             hintColor = theme.colors.on_surface_variant] {
        newName = huxerui::TextEditingValue{""};
        newUrl = huxerui::TextEditingValue{""};
        newTypeIdx = 0;
        newDesc = huxerui::TextEditingValue{""};
        newTimeout = huxerui::TextEditingValue{""};
        newInterval = huxerui::TextEditingValue{""};
        newAuto = false;
        newSys = false;
        newCore = false;
        newCert = false;
        newPptpServer = huxerui::TextEditingValue{""};
        newPptpUsername = huxerui::TextEditingValue{""};
        newPptpPassword = huxerui::TextEditingValue{""};
        newPptpTimeout = huxerui::TextEditingValue{"30"};
        newPptpRoutes = huxerui::TextEditingValue{""};
        newPptpMppe = true;
        newOpenVpnConfig = huxerui::TextEditingValue{""};
        newOpenVpnRoutes = huxerui::TextEditingValue{""};
        pickedPath = "";
        dialog.Show(
            [tasks, toast, picker, newName, newUrl, newTypeIdx, newDesc,
             newTimeout, newInterval, newAuto, newSys, newCore, newCert,
             newPptpServer, newPptpUsername, newPptpPassword, newPptpTimeout,
             newPptpRoutes, newPptpMppe, newOpenVpnConfig, newOpenVpnRoutes,
             pickedPath, importing, pptpSupported, openvpnSupported,
             hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                const std::size_t typeIndex = newTypeIdx.Get();
                const bool remote = typeIndex == 0;
                const bool local = typeIndex == 1;
                const bool pptp = pptpSupported && typeIndex == 2;
                const bool openvpn = openvpnSupported &&
                                     typeIndex == (pptpSupported ? 3 : 2);
                huxerui::View typeFields;
                if (remote) {
                    typeFields = huxerui::TextField(newUrl.Get())
                                     .Label("订阅链接")
                                     .Placeholder("https://...")
                                     .Variant(huxerui::TextFieldVariant::Outlined)
                                     .OnChanged([newUrl](
                                                    const huxerui::TextEditingValue& v) {
                                         newUrl = v;
                                     });
                } else if (local) {
                    typeFields = huxerui::Row {
                        huxerui::Button("选择文件")
                            .OnClick([tasks, picker, pickedPath] {
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const auto picked = co_await picker->OpenFileAsync(
                                        huxerui::FilePickerFilter{
                                            .name = "YAML 订阅",
                                            .extensions = {"yaml", "yml"}});
                                    if (!picked) co_return;
                                    if (const auto f = picked->AsFile()) {
                                        pickedPath = f->Path();
                                    } else {
                                        pickedPath = "";
                                    }
                                });
                            }),
                        huxerui::Text(pickedPath.Get())
                            .Style(huxerui::TextStyle{
                                huxerui::Font::Monospace(font_size::kChip),
                                hintColor}),
                    }.With(huxerui::Spacing(8.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center));
                } else if (pptp) {
                    typeFields = PptpOptionsForm(
                        newPptpServer, newPptpUsername, newPptpPassword,
                        newPptpTimeout, newPptpRoutes, newPptpMppe);
                } else if (openvpn) {
                    typeFields = OpenVpnOptionsForm(newOpenVpnConfig,
                                                    newOpenVpnRoutes);
                } else {
                    typeFields = huxerui::TextField(newUrl.Get())
                                     .Label("订阅链接")
                                     .Placeholder("https://...")
                                     .Variant(huxerui::TextFieldVariant::Outlined)
                                     .OnChanged([newUrl](
                                                    const huxerui::TextEditingValue& v) {
                                         newUrl = v;
                                     });
                }
                huxerui::View commonFields =
                    (pptp || openvpn) ? huxerui::View{huxerui::TextField(newDesc.Get())
                                              .Label("描述（可选）")
                                              .Variant(huxerui::TextFieldVariant::Outlined)
                                              .OnChanged([newDesc](
                                                             const huxerui::TextEditingValue& v) {
                                                  newDesc = v;
                                              })}
                         : huxerui::View{ProfileOptionsForm(
                               newDesc, newTimeout, newInterval, newAuto, newSys,
                               newCore, newCert)};
                return DialogCard(huxerui::Column {
                    huxerui::Text("新建订阅", huxerui::TextRole::Title),
                    huxerui::ScrollView(huxerui::Column {
                        huxerui::SegmentedButton(
                            [&]() {
                                std::vector<huxerui::StringVariant> types{
                                    "远程订阅", "本地文件"};
                                if (pptpSupported) types.emplace_back("PPTP 内网");
                                if (openvpnSupported) types.emplace_back("OpenVPN 内网");
                                return types;
                            }(),
                            newTypeIdx.Get())
                            .OnChanged([newTypeIdx](std::size_t idx) {
                                newTypeIdx = idx;
                            }),
                        std::move(typeFields),
                        huxerui::TextField(newName.Get())
                            .Label("名称（可选）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([newName](const huxerui::TextEditingValue& v) {
                                newName = v;
                            }),
                        std::move(commonFields),
                    }
                                       .With(huxerui::Spacing(12.0F),
                                             huxerui::CrossAlign(
                                                 huxerui::CrossAxisAlignment::Stretch)))
                        .With(huxerui::Frame{.height = kDialogFormHeight}),
                    huxerui::Row {
                        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                        importing.Get()
                            ? huxerui::View{
                                  huxerui::ProgressCircle()
                                      .With(huxerui::Frame{.width = 20.0F,
                                                           .height = 20.0F})}
                            : huxerui::View{
                                  huxerui::Button("导入").OnClick([=] {
                                      const std::string url = newUrl.Get().text;
                                      if (remote && url.empty()) {
                                          toast.Show("订阅链接不能为空");
                                          return;
                                      }
                                      if (local && pickedPath.Get().empty()) {
                                          toast.Show("请先选择订阅文件");
                                          return;
                                      }
                                      std::string configError;
                                      std::optional<std::string> nativeConfig;
                                      if (pptp) {
                                          nativeConfig = makePptpConfig(
                                              newPptpServer.Get(), newPptpUsername.Get(),
                                              newPptpPassword.Get(), newPptpTimeout.Get(),
                                              newPptpMppe.Get(), configError);
                                          if (!nativeConfig) {
                                              toast.Show(configError);
                                              return;
                                          }
                                      } else if (openvpn) {
                                          if (!openvpn::ParseOpenVpnConfig(
                                                  newOpenVpnConfig.Get().text,
                                                  configError)) {
                                              toast.Show(configError);
                                              return;
                                          }
                                          nativeConfig = newOpenVpnConfig.Get().text;
                                      }
                                      importing = true;
                                      tasks.Launch([=]() -> huxerui::Task<void> {
                                          db::Profile options;
                                          options.description =
                                              newDesc.Get().text;
                                          if (!pptp && !openvpn) {
                                              options.timeoutSecs =
                                                  parseNumber(newTimeout.Get(), 60);
                                              options.intervalMins =
                                                  parseNumber(newInterval.Get(), 0);
                                              options.autoUpdate = newAuto.Get();
                                              options.useSystemProxy = newSys.Get();
                                              options.useCoreProxy = newCore.Get();
                                              options.allowInvalidCert = newCert.Get();
                                          } else {
                                              options.type = pptp ? "pptp" : "openvpn";
                                              options.nativeConfig = *nativeConfig;
                                              options.nativeRoutes = joinRoutes(
                                                  parseRouteField(pptp
                                                                      ? newPptpRoutes.Get().text
                                                                      : newOpenVpnRoutes.Get().text));
                                          }
                                          const std::string name =
                                              newName.Get().text;
                                          const auto [rid, err] = co_await
                                              RunOnTaskThread(
                                                  [remote, local, pptp, openvpn, name, url, options,
                                                   picked = pickedPath.Get()] {
                                                      auto& ps =
                                                          store::profilesStore();
                                                      const std::int64_t nid =
                                                          remote
                                                              ? ps.importUrl(
                                                                    name, url,
                                                                    options)
                                                              : local
                                                                    ? ps.importFile(name, picked,
                                                                                     options)
                                                                    : ps.importNative(
                                                                          name,
                                                                          pptp ? "pptp" : "openvpn",
                                                                          options.nativeConfig,
                                                                          options.nativeRoutes,
                                                                          options);
                                                      return std::pair{
                                                          nid, ps.lastError()};
                                                  });
                                          importing = false;
                                          if (rid == 0) {
                                              toast.Show(
                                                  err.empty() ? "导入失败" : err);
                                          } else {
                                              ctx.Dismiss();
                                              toast.Show("订阅已导入");
                                          }
                                      });
                                  })},
                    }.With(huxerui::MainAlign(
                               huxerui::MainAxisAlignment::SpaceBetween),
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Center)),
                }
                                  .With(huxerui::Spacing(12.0F),
                                        huxerui::Frame{.width = 420.0F},
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    // 只生成轻量级的分组索引，真实 Profile 仍由 StateList 持有，卡片仍由
    // VirtualGrid 懒加载；这样分组不会退化成预先组合整个 View 列表。
    std::vector<std::string> profileTypes{"remote", "local", "pptp", "openvpn"};
    for (const db::Profile& profile : profiles) {
        if (std::find(profileTypes.begin(), profileTypes.end(), profile.type) ==
            profileTypes.end()) {
            profileTypes.push_back(profile.type);
        }
    }

    std::vector<ProfileGridItem> profileItems;
    std::vector<std::size_t> profileSpans;
    constexpr std::size_t kFullRowSpan = std::numeric_limits<std::size_t>::max();
    for (const std::string& type : profileTypes) {
        std::size_t count = 0;
        for (const db::Profile& profile : profiles) {
            if (profile.type == type) ++count;
        }
        if (count == 0) continue;

        profileItems.push_back(ProfileGridItem{
            .kind = ProfileGridItemKind::GroupHeader,
            .type = type,
            .profileCount = count,
        });
        profileSpans.push_back(kFullRowSpan);
        for (std::size_t index = 0; index < profiles.Size(); ++index) {
            if (profiles[index].type != type) continue;
            profileItems.push_back(ProfileGridItem{
                .kind = ProfileGridItemKind::Profile,
                .type = type,
                .profileIndex = index,
            });
            profileSpans.push_back(1);
        }
    }
    if (compact) {
        profileItems.push_back(ProfileGridItem{
            .kind = ProfileGridItemKind::Footer,
        });
        profileSpans.push_back(kFullRowSpan);
    }

    huxerui::View profileGrid = huxerui::VirtualGrid(
                                    profileItems.size(),
                                    [profiles, profileItems, compact, menu, tasks, toast, reload,
                                     pptpStates, openVpnStates, isConnectionSelected,
                                     toggleConnection, openEditInfo, showEditRules,
                                     showEditFile, showQr, theme](
                                        std::size_t index)
                                         -> huxerui::View {
                                        const ProfileGridItem& item = profileItems[index];
                                        if (item.kind == ProfileGridItemKind::Footer) {
                                            return CompactFloatingNavigationFooter()
                                                .Key("compact-floating-footer");
                                        }
                                        if (item.kind == ProfileGridItemKind::GroupHeader) {
                                            return huxerui::Row{
                                                huxerui::Text(
                                                    profileTypeLabel(item.type),
                                                    huxerui::TextRole::Title),
                                                huxerui::Spacer{},
                                                huxerui::Text(std::format(
                                                    "{} 个", item.profileCount)),
                                            }
                                                .With(
                                                    huxerui::Padding(
                                                        huxerui::EdgeInsets::Symmetric(
                                                            theme.spacing.small,
                                                            theme.spacing.extra_small)),
                                                    huxerui::Background(
                                                        theme.colors.surface_container_low),
                                                    huxerui::CornerRadius(12.0F),
                                                    huxerui::CrossAlign(
                                                        huxerui::CrossAxisAlignment::Center))
                                                .Key("profile-group-" + item.type);
                                        }

                                        const db::Profile& profile =
                                            profiles[item.profileIndex];
                                        store::PptpState pptpState =
                                            profile.type == "pptp"
                                                ? store::vpnStore().state(profile.id)
                                                : store::PptpState{};
                                        for (const auto& state : pptpStates) {
                                            if (state.profileId == profile.id) {
                                                pptpState = state;
                                                break;
                                            }
                                        }
                                        store::OpenVpnState openVpnState =
                                            profile.type == "openvpn"
                                                ? store::vpnStore().openVpnState(profile.id)
                                                : store::OpenVpnState{};
                                        for (const auto& state : openVpnStates) {
                                            if (state.profileId == profile.id) {
                                                openVpnState = state;
                                                break;
                                            }
                                        }
                                        return ProfileCard(
                                                   profile, compact, menu, tasks,
                                                   toast, reload, pptpState, openVpnState,
                                                   isConnectionSelected(profile.id),
                                                   toggleConnection, openEditInfo,
                                                   showEditRules, showEditFile, showQr)
                                            .Key(profile.id);
                                    })
                                    .Columns(compact
                                                 ? huxerui::GridColumns::Fixed(1)
                                                 : huxerui::GridColumns::Adaptive(
                                                       kCardWidth))
                                    .EstimatedRowExtent(kCardHeight)
                                    .ItemSpans(std::move(profileSpans))
                                    .RowSpacing(theme.spacing.medium)
                                    .ColumnSpacing(theme.spacing.medium)
                                    .With(huxerui::Grow(1.0F), huxerui::ScrollBar());

    huxerui::View batchActions = huxerui::Row{};
    if (connectionSelection.Size() > 0) {
        batchActions = huxerui::Row {
            huxerui::Text(std::format("已选 {} 个连接", connectionSelection.Size())),
            huxerui::Button("打开").OnClick(openSelectedConnections),
            huxerui::Button("关闭").OnClick(closeSelectedConnections),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    huxerui::View profileListPage = PageScaffold(
        "订阅",
        huxerui::Row {
            std::move(batchActions),
            huxerui::Button("新建订阅").OnClick(
                [tasks, showCreateDialog] {
                    // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        showCreateDialog();
                    });
                }),
        },
        profiles.Empty()
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("还没有订阅。点击右上角「新建订阅」导入。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : std::move(profileGrid));

    huxerui::View mobileEditPage = PlatformControl(
        {PlatformCode::Android},
        [editPageId, tasks, toast, editName, editUrl, editType, editDesc,
         editTimeout, editInterval, editAuto, editSys, editCore, editCert,
         editPptpServer, editPptpUsername, editPptpPassword, editPptpTimeout,
         editPptpRoutes, editPptpMppe, editOpenVpnConfig, editOpenVpnRoutes] {
            const std::int64_t id = editPageId.Get();
            if (id == 0) return huxerui::View{};
            return ProfileEditPage(
                id, editName, editUrl, editType, editDesc, editTimeout,
                editInterval, editAuto, editSys, editCore, editCert,
                editPptpServer, editPptpUsername, editPptpPassword,
                editPptpTimeout, editPptpRoutes, editPptpMppe, editOpenVpnConfig,
                editOpenVpnRoutes, tasks, toast,
                [editPageId] { editPageId = 0; });
        });

    return huxerui::IndexedPages(
               std::vector<huxerui::View>{std::move(profileListPage),
                                          std::move(mobileEditPage)},
               isAndroid && editPageId.Get() != 0 ? 1U : 0U)
        .With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
