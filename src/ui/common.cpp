// common.cpp — 岛屿原语（ResolveIslandTheme/IslandSurface/IslandSection）、
// 页面骨架（一级岛）/ 卡片（二级岛）等跨页通用部件。
#include <huxerui/huxerui.h>
#include <huxerui/file_drop.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "app_resources.h"
#include "proxies_model.h"
#include "task_bridge.h"
#include "ui.h"
#include "page_layout.h"
#include "title_bar_search.h"
#include "responsive_shell.h"
#include "app.h"
#include "file_drop_zone.h"
#include "profile_file_picker.h"

#include "wire_codec.h"
import clashflux.config;
import clashflux.core;
import clashflux.db;
import clashflux.persistence;
import clashflux.service;
import clashflux.singbox;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须放在模块
// 导入之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"
#include "profiles_model.h"
#include "settings_model.h"
#include "app_runtime_services.h"
#include "stream_updates.h"
#include "vpn_model.h"

namespace clashflux::ui {

namespace {

bool isSelectorType(const std::string& type) {
    return type == "Selector" || type == "selector";
}

enum class CoreApplyStatus { Applied, PermissionDenied, ServiceInstallFailed, Failed };
struct CoreApplyResult {
    CoreApplyStatus status = CoreApplyStatus::Failed;
    std::string error;
};

CoreApplyResult ApplyDesktopSystemProxy(bool enabled) {
    auto& coreStore = store::coreStore();
    if (coreStore.applySystemProxy(enabled)) {
        return {.status = CoreApplyStatus::Applied};
    }
    return {.status = CoreApplyStatus::Failed,
            .error = coreStore.snapshot().lastError};
}

CoreApplyResult ApplyDesktopTun(bool enabled) {
    if (enabled) {
        switch (core::tunGate()) {
            case core::TunGate::Ok:
                break;
            case core::TunGate::Denied:
                return {.status = CoreApplyStatus::PermissionDenied};
        }
    }

    auto& coreStore = store::coreStore();
    if (coreStore.applyTun(enabled)) {
        return {.status = CoreApplyStatus::Applied};
    }
    return {.status = CoreApplyStatus::Failed,
            .error = coreStore.snapshot().lastError};
}

CoreApplyResult ExecuteCoreCommand(const CoreCommand& command) {
    auto& coreStore = store::coreStore();
    bool ok = false;
    switch (command.action) {
        case CoreAction::Mode:
            if (command.mode != "rule" && command.mode != "global" && command.mode != "direct")
                return {.error = "无效的出站模式"};
            ok = coreStore.applyMode(command.mode);
            break;
        case CoreAction::SystemProxy:
            return ApplyDesktopSystemProxy(command.enabled);
        case CoreAction::Tun:
#if defined(__ANDROID__)
            coreStore.setSetting("core.tun_enabled", command.enabled ? "true" : "false");
            if (!command.enabled) {
                if (!AndroidStopVpn()) {
                    return {.status = CoreApplyStatus::Failed,
                            .error = "Android 后台服务未确认内核退出"};
                }
            }
            coreStore.startCore(store::profilesStore().selectedYaml(), false, command.enabled);
            // Authorization/service completion is awaited by the application task.
            return {.status = CoreApplyStatus::Applied};
#else
            return ApplyDesktopTun(command.enabled);
#endif
        case CoreAction::Stop:
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
            ok = coreStore.stopCore();
#else
            {
                std::string error;
                const bool stopped = store::vpnStore().stopTraffic(error);
                return {.status = stopped ? CoreApplyStatus::Applied
                                          : CoreApplyStatus::Failed,
                        .error = std::move(error)};
            }
#endif
            break;
        case CoreAction::Restart:
#if defined(_WIN32)
            if (!service::installed()) {
                return {.status = CoreApplyStatus::PermissionDenied};
            }
#endif
            ok = coreStore.restartCore(store::profilesStore().selectedYaml());
            break;
        case CoreAction::Start:
#if defined(_WIN32)
            if (!service::installed()) {
                return {.status = CoreApplyStatus::PermissionDenied};
            }
#endif
            coreStore.startCore(store::profilesStore().selectedYaml(), false,
                                coreStore.tunEnabled(), coreStore.systemProxyEnabled());
            ok = coreStore.snapshot().state == core::CoreState::Running &&
                 coreStore.snapshot().lastError.empty();
            break;
        case CoreAction::Idle:
            return {};
    }
    return {.status = ok ? CoreApplyStatus::Applied : CoreApplyStatus::Failed,
            .error = ok ? std::string{} : coreStore.snapshot().lastError};
}

huxerui::StringVariant CoreActionFailure(CoreAction action) {
    switch (action) {
        case CoreAction::Start: return Localized("启动内核失败");
        case CoreAction::Stop: return Localized("停止内核失败");
        case CoreAction::Restart: return Localized("启动内核失败");
        case CoreAction::Mode: return Localized("出站模式切换失败");
        case CoreAction::SystemProxy: return Localized("系统代理设置失败");
        default: return Localized("TUN 切换失败");
    }
}

} // namespace

void CoreControl::Run(CoreCommand command) const {
    if (Busy() || command.action == CoreAction::Idle) return;
    model->diagnosticsCancel.request_stop();
    const CoreControl control = *this;
    const bool previousTun = model->view.Get().tunIntent;
    const CoreAction action = command.action;
    LaunchCoreOperation<CoreApplyResult>(tasks, model, std::move(command),
        [control, previousTun](CoreCommand command) -> huxerui::Task<CoreApplyResult> {
            CoreApplyResult result;
            try {
                result = co_await RunOnTaskThread([command] { return ExecuteCoreCommand(command); });
    #if defined(__linux__) || defined(_WIN32)
                if (result.status == CoreApplyStatus::PermissionDenied &&
                    control.window && control.dialog && control.toast &&
                    (!control.presentationAvailable || control.presentationAvailable()) &&
                    !co_await RunOnTaskThread([] { return service::installed(); })) {
                    control.window->Show();
                    control.window->Activate();
                    const auto resumed = co_await ResumeCoreCommandAfterServiceInstall<CoreApplyResult>(
                        command, [&result, control]() -> huxerui::Task<bool> {
                            const auto installed = co_await InstallDesktopServiceAsync();
                            result.error = installed.error;
                            if (!installed.success || !installed.installed) co_return false;
                            control.toast->Show(Localized("服务已安装"));
                            co_return true;
                        }, [](CoreCommand original) -> huxerui::Task<CoreApplyResult> {
                            co_return co_await RunOnTaskThread(
                                [original] { return ExecuteCoreCommand(original); });
                        });
                    if (resumed) result = *resumed;
                    else result.status = CoreApplyStatus::ServiceInstallFailed;
                }
    #endif
    #if defined(__ANDROID__)
                if (command.action == CoreAction::Tun && result.status == CoreApplyStatus::Applied) {
                    bool accepted = false;
                    for (int attempt = 0; attempt < 300; ++attempt) {
                        const auto [state, requested] = co_await RunOnTaskThread([] {
                            return std::pair{AndroidVpnState(), store::coreStore().tunEnabled()};
                        });
                        const bool active = state == 1 || state == 2;
                        if (command.enabled ? active : (!active && !requested)) {
                            accepted = true;
                            break;
                        }
                        if (command.enabled && (state == 3 || !requested)) break;
                        co_await huxerui::Delay(std::chrono::duration<double>{0.2});
                    }
                    if (!accepted) result.status = CoreApplyStatus::Failed;
                }
    #else
                static_cast<void>(previousTun);
    #endif
            } catch (const std::exception& error) {
                result.error = error.what();
                result.status = CoreApplyStatus::Failed;
            } catch (...) {
                result.status = CoreApplyStatus::Failed;
            }
            try {
    #if defined(__ANDROID__)
                if (command.action == CoreAction::Tun && result.status != CoreApplyStatus::Applied) {
                    co_await RunOnTaskThread([command, previousTun] {
                        auto& coreStore = store::coreStore();
                        // Do not overwrite a newer service/tile intent or a connected tunnel.
                        if (coreStore.tunEnabled() == command.enabled && AndroidVpnState() != 2)
                            coreStore.setSetting("core.tun_enabled", previousTun ? "true" : "false");
                    });
                }
    #endif
                for (;;) {
                    const auto revision = control.model->operations.revision.Get();
                    auto next = co_await RunOnTaskThread([] { return ReadCoreView(true, false); });
                    if (control.model->operations.revision.Get() != revision) continue;
                    control.model->Update([next = std::move(next)](CoreView& view) { view = next; });
                    break;
                }
            } catch (const std::exception& error) {
                if (result.error.empty()) result.error = error.what();
                result.status = CoreApplyStatus::Failed;
            } catch (...) {
                result.status = CoreApplyStatus::Failed;
            }
            co_return result;
    }, [control, action](std::optional<CoreApplyResult> completed, std::exception_ptr error) {
        CoreApplyResult result = completed.value_or(CoreApplyResult{});
        if (error) {
            try { std::rethrow_exception(error); }
            catch (const std::exception& exception) { result.error = exception.what(); }
            catch (...) {}
        }
        control.model->RequestRefresh();
        if (control.presentationAvailable && !control.presentationAvailable()) {
            if (result.status != CoreApplyStatus::Applied && control.onHeadlessFailure)
                control.onHeadlessFailure(
                    result.status == CoreApplyStatus::PermissionDenied, result.error);
            return;
        }
        if (result.status == CoreApplyStatus::PermissionDenied) {
            if (!control.window || !control.dialog || !control.toast) {
                if (control.onHeadlessFailure)
                    control.onHeadlessFailure(true, result.error);
                return;
            }
            control.window->Show();
            control.window->Activate();
            ShowTunGuideDialog(*control.dialog, control.clipboard, *control.toast,
                               control.textColor, control.hintColor);
        } else if (result.status != CoreApplyStatus::Applied) {
            if (control.toast) {
                control.toast->Show(!result.error.empty() ? huxerui::StringVariant(result.error)
                    : result.status == CoreApplyStatus::ServiceInstallFailed
                        ? Localized("安装被取消或失败（需要授权）") : CoreActionFailure(action));
            } else if (control.onHeadlessFailure) {
                control.onHeadlessFailure(false, result.error);
            }
        }
    });
}

void CoreModel::SetAllowLan(bool enabled) {
    if (!view.Get().ready) return;
    store::coreStore().setSetting("core.allow_lan", enabled ? "true" : "false");
    Update([enabled](CoreView& next) { next.allowLan = enabled; });
}

void CoreModel::SetIpv6(bool enabled) {
    if (!view.Get().ready) return;
    store::coreStore().setSetting("core.ipv6_enabled", enabled ? "true" : "false");
    Update([enabled](CoreView& next) { next.ipv6Enabled = enabled; });
}

PortSettingsError SettingsModel::SetPorts(std::string mixed, std::string http, std::string socks) {
    if (!view.Get().ready) return PortSettingsError::NotReady;
    const auto parsed = ParseInboundPorts(mixed, http, socks);
    if (parsed.error != PortSettingsError::None) return parsed.error;
    const auto& ports = parsed.ports;
    auto& coreStore = store::coreStore();
    coreStore.setSetting("core.mixed_port", ports.mixed);
    coreStore.setSetting("core.http_port", ports.http);
    coreStore.setSetting("core.socks_port", ports.socks);
    Update([&](SettingsView& next) {
        next.mixedPort = ports.mixed;
        next.httpPort = ports.http;
        next.socksPort = ports.socks;
    });
    return PortSettingsError::None;
}

void SettingsModel::SetEnvShell(std::string shell) {
    if (!view.Get().ready) return;
    store::coreStore().setSetting("ui.env_shell", shell);
    Update([&](SettingsView& next) { next.envShell = shell; });
}

namespace {

// Clash 来源的运行 tag 可逆。对象目录尚未发布时先显示来源内原名，
// 避免把 cf_<来源十六进制>_o_<名称十六进制> 暴露给用户。
std::optional<std::string> DecodeSourceOutboundName(std::string_view tag) {
    if (!tag.starts_with("cf_")) return std::nullopt;
    const auto separator = tag.find("_o_", 3);
    if (separator == std::string_view::npos) return std::nullopt;
    const auto decode = [](std::string_view hex) -> std::optional<std::string> {
        if (hex.empty() || hex.size() % 2 != 0) return std::nullopt;
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        std::string result;
        result.reserve(hex.size() / 2);
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            const int high = digit(hex[i]);
            const int low = digit(hex[i + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            const char value = static_cast<char>((high << 4) | low);
            if (static_cast<unsigned char>(value) < 0x20) return std::nullopt;
            result += value;
        }
        return result;
    };
    const auto source = decode(tag.substr(3, separator - 3));
    if (!source || !source->starts_with("profile-") || source->size() == 8 ||
        !std::ranges::all_of(source->substr(8), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return decode(tag.substr(separator + 3));
}

// 只服务于 FetchProxiesSnapshot：拥有型策略组 DTO → 扁平快照。不再是公开 API——
// 消费方一律读 ProxiesModel，避免又出现第二个「各自解析」的入口。
std::vector<ProxyGroupSnapshot> ParseProxyGroups(const wire::Proxies& snapshot, const std::map<std::string, std::string>& labels) {
    std::vector<ProxyGroupSnapshot> groups;
    const auto& proxies = snapshot.entries;
    for (const auto& [name, value] : proxies) {
        if (!value.members) continue;
        ProxyGroupSnapshot group;
        group.name = name;
        group.displayName = labels.contains(name) ? labels.at(name) : name;

        group.type = value.type;
        group.current = value.now;
        group.selectable = value.selectable.value_or(isSelectorType(group.type));
        group.nodes = *value.members;
        for (const auto& node : group.nodes) {
            if (labels.contains(node)) group.nodeLabels[node] = labels.at(node);
            if (const auto found = proxies.find(node); found != proxies.end()) {
                group.delays[node] = found->second.historyDelay;
            }
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

} // namespace

std::vector<huxerui::MenuEntry> BuildProxyLineMenu(
    const std::vector<ProxyGroupSnapshot>& groups,
    std::function<void(const std::string&, const std::string&)> on_select,
    const std::function<std::string(huxerui::StringVariant)>& resolveLabel) {
    std::vector<huxerui::MenuEntry> entries;
    for (const ProxyGroupSnapshot& group : groups) {
        if (!group.selectable) {
            entries.push_back(huxerui::MenuItem(
                resolveLabel(LocalizedFormat("{}（自动测速，不支持手动切换）", group.displayName)),
                [] {}).Enabled(false));
            continue;
        }
        std::vector<huxerui::MenuEntry> nodes;
        for (const std::string& node : group.nodes) {
            nodes.push_back(huxerui::MenuItem(
                                group.nodeLabels.contains(node) ? group.nodeLabels.at(node) : node,
                                [on_select, groupName = group.name, node] {
                                    on_select(groupName, node);
                                })
                                .Checked(node == group.current));
        }
        if (nodes.empty()) {
            nodes.push_back(
                huxerui::MenuItem(resolveLabel(Localized("暂无可切换线路")), [] {}).Enabled(false));
        }
        entries.push_back(
            huxerui::MenuItem(group.displayName, std::move(nodes)));
    }
    if (entries.empty()) {
        entries.push_back(
            huxerui::MenuItem(resolveLabel(Localized("暂无可切换线路")), [] {}).Enabled(false));
    }
    return entries;
}

// ---- 策略组快照：唯一来源（见 proxies_model.h）------------------------------

ProxiesSnapshot FetchProxiesSnapshot() {
    ProxiesSnapshot next;
    next.loaded = true;
    std::string body;
#if defined(__ANDROID__)
    const char* raw = clashflux_android_proxy_groups();
    // VPN 服务未启动时 Java 侧没有 libbox CommandClient，会返回空组对象；
    // 此时回落到原生 store 的「停止内核」订阅预览，并标明这是预览而非实时。
    if (raw != nullptr && *raw != '\0' &&
        std::string_view(raw) != "{\"proxies\":{}}") {
        body = raw;
        next.source = ProxiesSource::Live;
    } else {
        body = store::coreStore().proxyGroupsSnapshot();
        next.source = body.empty() ? ProxiesSource::Empty : ProxiesSource::Preview;
    }
#else
    const auto result = store::coreStore().api().proxies();
    if (result.ok) {
        body = result.body;
        next.source = ProxiesSource::Live;
    } else {
        // 回落是刻意的（停内核时仍要让用户能预先选节点），但把原因记下来，
        // 页面可以据此提示「当前是预览」，而不是把预览当实时数据展示。
        body = store::coreStore().proxyGroupsSnapshot();
        next.source = body.empty() ? ProxiesSource::Empty : ProxiesSource::Preview;
        next.error = result.error;
    }
#endif
    next.body = std::move(body);
    if (!next.body.empty()) {
        auto decoded = wire::DecodeProxies(next.body);
        if (decoded) {
            next.proxies = std::make_shared<const wire::Proxies>(std::move(decoded.value));
            const auto catalog = store::coreStore().proxySourceObjects(next.source == ProxiesSource::Preview);
            if (catalog) for (const auto& object : *catalog) {
                next.labels[object.tag] = object.sourceName.empty() ? object.objectId
                    : object.objectId + " · " + object.sourceName;
            }
            for (const auto& entry : next.proxies->entries) {
                const auto& tag = entry.first;
                if (next.labels.contains(tag)) continue;
                if (const auto name = DecodeSourceOutboundName(tag)) next.labels[tag] = *name;
            }
            next.groups = ParseProxyGroups(*next.proxies, next.labels);
        } else {
            next.error = "代理快照解析失败：" + decoded.error;
            next.source = ProxiesSource::Empty;
        }
    }
    return next;
}

void InstallClashFluxUiModels(huxerui::ApplicationContext& context) {
    auto runtimeServices = std::make_shared<AppRuntimeServices>();
    context.Provide(runtimeServices);
    context.Provide(std::make_shared<ProxiesModel>());
    context.Provide(std::make_shared<CoreModel>());
    auto profiles = std::make_shared<ProfilesModel>();
    context.Provide(profiles);
    InstallProfileLinkActivation(context, profiles, runtimeServices);
    context.Provide(std::make_shared<VpnModel>());
    context.Provide(std::make_shared<SettingsModel>());
}

void DriveProxiesModel(huxerui::TaskScope tasks,
                       std::shared_ptr<ProxiesModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        for (;;) {
            const std::uint64_t before = model->refreshTick.Get();
            ProxiesSnapshot next = co_await RunOnTaskThread(FetchProxiesSnapshot);
            // 取数据在 worker，写 State 恒在 UI 线程；内容相等时 State::Write
            // 去重，不会引起订阅方重组。
            model->snapshot = std::move(next);
            if (model->refreshTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{2.0});
        }
    });
}

// ---- 内核/接管状态：唯一来源（见 core_model.h）------------------------------

CoreView ReadCoreView(bool slowProbes, bool previousSystemProxyActive) {
    auto& core = store::coreStore();
    // 内核崩溃检测：Running 但进程/控制器已不在 → Failed。以前由托盘的 0.5s
    // 泵顺带做，现在集中在这里每秒做一次。
    core.checkAlive();
    core.captureHealthEvidence(slowProbes);
    CoreView view;
    view.core = core.snapshot();
    view.health = core.runtimeHealthSnapshot(view.core);
    view.ready = clashflux::persistence::persistence().ready();
    view.tunIntent = core.tunEnabled();
    view.modeIntent = core.mode();
    const auto vpnRuntime = ReadAndroidVpnRuntime();
    view.vpnSnapshotReady = vpnRuntime.ready;
    view.vpnState = vpnRuntime.state;
    view.vpnElapsedMs = vpnRuntime.elapsedMs;
    view.ignoringBattery = AndroidIsIgnoringBattery();
    view.systemProxyIntent = core.systemProxyEnabled();
    // 系统代理实际指向在 GNOME 下是 3 次 popen：只在慢探测拍或动作请求时查，
    // 其余时候沿用上一次的结果（State 只能在 UI 线程读，故由调用方按值传入）。
    view.systemProxyActive = slowProbes ? core.systemProxyActive()
                                        : previousSystemProxyActive;
    view.ipv6Enabled = core.setting("core.ipv6_enabled", "false") == "true";
    // 局域网开关显示的是「意图」（KV），不是内核运行时的 allow-lan 回读值：
    // 内核没跑时运行时快照不含本次意图，写透后会被这一拍打回去。
    view.allowLan = core.setting("core.allow_lan", "false") == "true";
    view.serviceInstalled = service::installed();
    return view;
}

void DriveCoreModel(huxerui::TaskScope tasks, std::shared_ptr<CoreModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        // 昂贵探测（系统代理实际指向）每 kSlowProbeInterval 拍做一次；显式的
        // RequestRefresh 在下一次可发布的读取时补做慢探测，不承担操作结果回传。
        constexpr int kSlowProbeInterval = 15;
        int slowCountdown = 0;
        std::uint64_t seenRefresh = 0;
        for (;;) {
            if (model->operations.Busy() || model->serviceChecking.Get()) {
                co_await huxerui::Delay(std::chrono::duration<double>{0.1});
                continue;
            }
            const std::uint64_t before = model->refreshTick.Get();
            const auto revision = model->operations.revision.Get();
            const bool slowProbes = slowCountdown <= 0 || before != seenRefresh;
            const CoreView previous = model->view.Get();
            CoreView next = co_await RunOnTaskThread(
                [slowProbes, active = previous.systemProxyActive] {
                    return ReadCoreView(slowProbes, active);
                });
            if (!model->PublishPolled(revision, std::move(next))) {
                co_await huxerui::Delay(std::chrono::duration<double>{0.1});
                continue;
            }
            seenRefresh = before;
            slowCountdown = slowProbes ? kSlowProbeInterval : slowCountdown - 1;
            if (model->refreshTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{1.0});
        }
    });
}

bool SelectProxyLine(const std::string& group, const std::string& name) {
#if defined(__ANDROID__)
    return store::coreStore().selectProxy(group, name);
#else
    return store::coreStore().selectProxy(group, name);
#endif
}

bool StartProxyGroupTest(const std::string& group) {
#if defined(__ANDROID__)
    try {
        // Android 的测速内核使用无 TUN 的 libbox 配置；如果当前没有已连接
        // 的 VPN 服务，先只编译当前配置，再由 Java 启动 speed-test-only
        // 服务。这样测速不会申请 VPN，也不会接管应用流量。
        if (clashflux_android_vpn_state() != 2) {
            auto& core = store::coreStore();
            core.startCore(store::profilesStore().selectedYaml(), false, false,
                           std::nullopt, true);
            if (core.snapshot().state == core::CoreState::Failed) return false;
        }
        return clashflux_android_url_test(group.c_str());
    } catch (...) {
        return false;
    }
#else
    // 桌面节点测速通过内核 delay API 完成。测速依赖内核但不需要接管流量，
    // 内核没跑时不再隐式拉起（内置启动入口只在首页右下角悬浮按钮与设置里的
    // 「启动时自动运行内核」），这里直接返回失败由页面提示先启动内核。
    if (store::coreStore().snapshot().state != core::CoreState::Running) {
        return false;
    }
    return TriggerProxyGroupTest(group);
#endif
}

[[huxerui::composable]] huxerui::View SettingItemIcon(huxerui::ImageResource icon, bool danger) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Image(icon)
        .Fit(huxerui::ImageFit::Contain)
        .Tint(danger ? theme.colors.error : theme.colors.on_surface_variant)
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F});
}

[[huxerui::composable]] huxerui::View SettingRow(huxerui::StringVariant label,
                                                 huxerui::StringVariant hint,
                                                 huxerui::View control,
                                                 std::optional<huxerui::ImageResource> icon) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View description = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        huxerui::UseString(hint).empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F));
    if (icon) {
        description = huxerui::Row {
          SettingItemIcon(*icon),
          std::move(description).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    if (compact) {
        huxerui::View compactControl = control;
        if (icon) compactControl = std::move(compactControl).With(huxerui::Padding(huxerui::EdgeInsets{.left = 32.0F}));
        return huxerui::Column {
          description,
          compactControl,
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    return huxerui::Row {
      std::move(description).With(huxerui::Grow(1.0F)),
      control,
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View SettingSwitchRow(
    huxerui::StringVariant label, huxerui::StringVariant hint, huxerui::View control,
    bool danger, std::optional<huxerui::ImageResource> icon) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::View text = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody),
            danger ? theme.colors.error : theme.colors.on_surface}),
        huxerui::UseString(hint).empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F));
    if (icon) {
        return huxerui::Row {
          SettingItemIcon(*icon, danger),
          text,
          control,
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    return huxerui::Row {
        std::move(text),
        control,
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View UnifiedListRow(
    huxerui::View content, std::string key, bool compact, bool divider) {
    const float horizontal = compact ? 10.0F : 8.0F;
    const float vertical = compact ? 8.0F : 6.0F;
    // 平铺列表行：不逐行套圆角卡壳，行直接落在一级岛表面上，行间用细分隔
    // 线划界；divider=false 用于最后一行，避免列表尾部悬一条分隔线。
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值 With 链。
    huxerui::View row = content;
    return huxerui::Column {
        std::move(row)
            .With(huxerui::Spacing(compact ? 5.0F : 8.0F),
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(horizontal,
                                                                   vertical)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        divider ? huxerui::View{huxerui::Divider()}
                : huxerui::View{huxerui::Row{}},
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        .Key(std::move(key));
}

[[huxerui::composable]] huxerui::View SectionTitle(huxerui::StringVariant title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

// 所有密码输入统一走 HuxerUI 的受控 TextField 显隐能力：Secure 负责安全
// 输入策略，TrailingIcon 负责内置眼睛操作。调用方只提供受控的完整
// TextEditingValue，秘密值不会被组件复制到日志、卡片或错误文本中。
[[huxerui::composable]] huxerui::View PasswordField(
    huxerui::State<huxerui::TextEditingValue> password) {
    auto visible = huxerui::UseState(false);
    const bool isVisible = visible.Get();
    return huxerui::TextField(password.Get())
        .Label(Localized("密码"))
        .Secure(!isVisible)
        .TrailingIcon(isVisible ? app::images::visibility_off
                                : app::images::visibility,
                      Localized(isVisible ? "隐藏密码" : "显示密码"))
        .OnTrailingIconClick([visible] { visible = !visible.Get(); })
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([password](const huxerui::TextEditingValue& value) {
            password = value;
        });
}

// TUN 权限引导弹窗（见 ui.h）。Linux 只引导安装服务模式：应用自身保持非 root
// （更安全），root 只在服务侧。命令行 = TextField 展示 + 复制按钮（runtime
// Clipboard 服务，组合期传入）。工厂 lambda 里只消费按值捕获的值，不放钩子
// （工厂在 layer 组合期执行，未经 codegen 管理的裸钩子是脆弱写法）。
void ShowTunGuideDialog(huxerui::DialogHandle dialog,
                        std::shared_ptr<huxerui::Clipboard> clipboard,
                        huxerui::ToastHandle toast, huxerui::Color textColor,
                        huxerui::Color hintColor) {
#if defined(__ANDROID__)
    (void)dialog;
    (void)clipboard;
    (void)textColor;
    (void)hintColor;
    toast.Show(Localized("请在首页通过悬浮按钮管理 VPN 连接"));
    return;
#elif defined(_WIN32)
    (void)clipboard;
    dialog.Show([textColor, hintColor](huxerui::DialogContext ctx) -> huxerui::View {
        return DialogCard(huxerui::Column {
            huxerui::Text(Localized("TUN 需要安装服务模式"), huxerui::TextRole::Title)
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody), textColor}),
            huxerui::Text(Localized("请到设置的内核服务安装或更新 Windows 服务，授权后再开启 TUN"))
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), hintColor}),
            huxerui::Button(Localized("关闭")).OnClick([ctx] { ctx.Dismiss(); }),
        }.With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 460.0F}));
    });
#else
    namespace fs = std::filesystem;
    const std::string exe = [] {
        const fs::path dir = cfg::executableDir();
        if (dir.empty()) return std::string{"clash-flux"};
        return (dir / "clash-flux").string();
    }();
    const std::string serviceCmd =
        std::format("pkexec {} service install", exe);
    dialog.Show(
        [clipboard, toast, textColor, hintColor,
         serviceCmd](huxerui::DialogContext ctx) -> huxerui::View {
            return DialogCard(huxerui::Column {
                huxerui::Text(Localized("TUN 需要安装服务模式"), huxerui::TextRole::Title),
                huxerui::Text(Localized(
                    "TUN 由内核创建虚拟网卡，需要 root 权限。应用本身"
                    "保持非 root 运行（更安全），由 root 服务托管内核。"
                    "复制指令到终端执行（pkexec 会弹出授权）后重试："))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption), hintColor}),
                huxerui::Text(Localized("安装 root 服务（内核由服务托管，TUN 开箱可用）"))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody), textColor}),
                huxerui::Row {
                    huxerui::TextField(
                        huxerui::TextEditingValue{serviceCmd})
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .With(huxerui::Grow(1.0F)),
                    huxerui::Button(Localized("复制")).OnClick([clipboard, toast,
                                                     serviceCmd] {
                        if (clipboard->WriteText(serviceCmd)) {
                            toast.Show(Localized("已复制到剪贴板"));
                        } else {
                            toast.Show(Localized("复制失败"));
                        }
                    }),
                }
                    .With(huxerui::Spacing(8.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center)),
                huxerui::Row {
                    huxerui::Button(Localized("关闭")).OnClick([ctx] { ctx.Dismiss(); }),
                }.With(huxerui::MainAlign(
                    huxerui::MainAxisAlignment::End)),
            }
                              .With(huxerui::Spacing(12.0F),
                                    huxerui::Frame{.width = 460.0F},
                                    huxerui::CrossAlign(
                                        huxerui::CrossAxisAlignment::Stretch)));
        },
        huxerui::DialogOptions{});
#endif
}

float ConcentricRadius(float outer_radius, float inset) {
    // 内层半径 = 外层半径 − inset，保下限 4pt：更小半径与父级圆角几乎相切，
    // 视觉上出现反同心（子角比父角“尖”）。
    constexpr float kMinRadius = 4.0F;
    return std::max(outer_radius - inset, kMinRadius);
}

namespace {

huxerui::Color IslandColor(const IslandTheme& islands, const huxerui::ThemeSpec& theme,
                           IslandLevel level) {
    switch (level) {
        case IslandLevel::Base: return islands.base;
        case IslandLevel::Raised: return islands.raised;
        case IslandLevel::Active: return islands.active;
        case IslandLevel::Overlay: return islands.overlay;
        case IslandLevel::Danger: {
            huxerui::Color danger = theme.colors.error;
            danger.alpha = 0.10F;
            return danger;
        }
    }
    return islands.base;
}

} // namespace

[[huxerui::composable]] huxerui::View WithoutIconButtonOutlines(
    huxerui::View content) {
    // The built-in icon-button focus ring and indication borders read these
    // scoped values; keep the rest of the theme and interaction feedback intact.
    huxerui::ThemeSpec theme = huxerui::UseTheme();
    theme.interactions.focus_ring =
        huxerui::FocusRing{huxerui::Color::Transparent(), 0.0F, 0.0F};
    const auto removeIndicationBorders = [](huxerui::Indication& indication) {
        const auto removeBorder = [](auto& layer) {
            if (layer) layer->border.reset();
        };
        removeBorder(indication.focus);
        removeBorder(indication.hover);
        removeBorder(indication.press);
    };
    removeIndicationBorders(theme.interactions.indication);

    huxerui::IconButtonStyle iconButtonStyle =
        huxerui::UseEnvironment<huxerui::IconButtonStyle>();
    if (iconButtonStyle.indication) {
        removeIndicationBorders(*iconButtonStyle.indication);
    }

    huxerui::ThemeDefinition overrides;
    overrides.Set(theme);
    overrides.Set(iconButtonStyle);
    return huxerui::Theme(overrides, content);
}

[[huxerui::composable]] huxerui::View IslandSurface(huxerui::View content,
                                                    IslandLevel level) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值 With 链。
    huxerui::View surface = content;
    return std::move(surface).With(huxerui::Background(IslandColor(islands, theme, level)),
                                   huxerui::CornerRadius(islands.island_radius),
                                   huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View IslandSection(std::string title,
                                                    huxerui::View content) {
    return IslandSurface(
        huxerui::Column {
            huxerui::Text(std::move(title), huxerui::TextRole::Title),
            std::move(content),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        IslandLevel::Base);
}

[[huxerui::composable]] huxerui::View PageScaffold(huxerui::StringVariant title,
                                                   huxerui::View actions,
                                                   huxerui::View content,
                                                   bool inlineCompactActions,
                                                   bool fullWidthSections,
                                                   bool windowTitle,
                                                   std::optional<float> contentSpacing,
                                                   huxerui::View leading,
                                                   huxerui::View titleSearchField,
                                                   bool showInlineHeader,
                                                   huxerui::View inlineHeader) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 响应式：Compact(<600) 收窄一级岛内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto contentInsets = PageContentInsets(theme, compact,
        fullWidthSections ? 0.0F : contentSpacing.value_or(compact ? 4.0F : kDesktopPageHorizontalInset));
    if (contentSpacing) contentInsets.top = *contentSpacing;
    if (kPageTitlesInWindow && !compact) {
        // 桌面动作直接挂在本页标题栏中，不经共享 State 转交 View 或复制回调。
        huxerui::View desktopBody = huxerui::View{content}.With(huxerui::Grow(1.0F));
        if (fullWidthSections) desktopBody = huxerui::ProvideEnvironment(
            SectionTabContentInsets{kSectionCardSpacing}, desktopBody);
        desktopBody = huxerui::ProvideEnvironment(
            SectionTabPickerInsets{kDesktopTitleBarHeight + kDesktopChromeDividerThickness},
            std::move(desktopBody));
        huxerui::View pageTitle = PageTitleText(theme, title).With(huxerui::Padding(
            huxerui::EdgeInsets{.left = leading ? 0.0F : kDesktopPageHorizontalInset}));
        // 搜索槽排在标题与动作之间，胶囊内部统一封顶 320pt，剩余空白用于拖动窗口；
        // 槽是行内唯一的 Grow 子项，窗口收窄时随标题/动作自动收敛；
        // 没有搜索框时保留弹性 Spacer 把动作推到最右。composable 形参被
        // codegen 固定为 const：拷贝到局部再走右值 With 链。
        huxerui::View actionRow = huxerui::Row{actions}.With(
            huxerui::Padding(huxerui::EdgeInsets{.right = 8.0F}),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        huxerui::View titleBarContent =
            titleSearchField
                ? huxerui::Row{
                      std::move(pageTitle),
                      huxerui::View{titleSearchField}.With(huxerui::Grow(1.0F)),
                      std::move(actionRow),
                  }
                : huxerui::Row{
                      std::move(pageTitle),
                      huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
                      std::move(actionRow),
                  };
        titleBarContent = std::move(titleBarContent).With(
            huxerui::Spacing(12.0F),
            huxerui::Grow(1.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        return huxerui::Column{
            huxerui::WindowTitleBar{
                huxerui::Row{leading}.With(huxerui::Padding(huxerui::EdgeInsets{
                    .left = leading ? kDesktopPageHorizontalInset : 0.0F})),
                std::move(titleBarContent),
            }.With(huxerui::Frame{.height = kDesktopTitleBarHeight}, huxerui::Spacing(0.0F)),
            huxerui::Row{}.With(huxerui::Frame{.height = kDesktopChromeDividerThickness},
                                huxerui::Background(theme.colors.outline)),
            DesktopTitleBarBody(huxerui::Column{std::move(desktopBody)}.With(
                huxerui::Padding(contentInsets),
                huxerui::Grow(1.0F),
                huxerui::Background(islands.base),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)), theme),
        }.With(huxerui::Spacing(0.0F), huxerui::Grow(1.0F),
               huxerui::ClipChildren(),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    static_cast<void>(windowTitle);
    // Compact 共用页内标题与动作；桌面窗口控件由外壳独立保留。
    huxerui::View titleView = showInlineHeader
        ? titleSearchField ? huxerui::View{titleSearchField} : PageTitleText(theme, title)
        : huxerui::View{};
    if (leading) titleView = huxerui::Row{leading, titleView}.With(
        huxerui::Spacing(theme.spacing.small),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    huxerui::View header = inlineHeader ? inlineHeader : PageHeaderLayout(theme,
        titleView,
        showInlineHeader ? huxerui::View{actions} : huxerui::View{}, compact && !inlineCompactActions,
        compact && static_cast<bool>(titleSearchField));
    const float horizontal = fullWidthSections
        ? kSectionCardSpacing
        : (compact ? 4.0F : kDesktopPageHorizontalInset);
    huxerui::View body = huxerui::View{content}.With(huxerui::Grow(1.0F));
    if (fullWidthSections) {
        if (header) header = huxerui::View{header}.With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                compact ? horizontal : kDesktopPageHorizontalInset, 0.0F)));
        body = huxerui::ProvideEnvironment(SectionTabContentInsets{horizontal}, body);
    }
    std::vector<huxerui::View> children;
    if (header) children.push_back(std::move(header));
    children.push_back(std::move(body));
    return huxerui::Column(std::move(children)).With(huxerui::Padding(contentInsets),
           huxerui::Spacing(contentSpacing.value_or(theme.spacing.medium)),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(kPageTitlesInWindow || compact ? 0.0F : islands.island_radius),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View SecondaryPageScaffold(
    huxerui::View title, huxerui::View actions, huxerui::View content,
    std::function<void()> onBack, bool hideBack, bool fullWidthSections,
    bool centerTitle) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View body = huxerui::View{content}.With(huxerui::Grow(1.0F));
    huxerui::View titleView = title;
    auto backAction = onBack;
    huxerui::View backButton;
    if (!hideBack) backButton =
        huxerui::IconButton(app::images::arrow_back, Localized("返回上一页"))
            .With(huxerui::Tooltip(Localized("返回上一页")))
            .OnClick(onBack);
    huxerui::View header = centerTitle
        ? CenteredPageHeader(theme, std::move(titleView), std::move(backButton), std::move(actions))
        : hideBack
        ? huxerui::Row {
              std::move(titleView).With(huxerui::Grow(1.0F)),
              std::move(actions),
          }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        : huxerui::Row {
              std::move(backButton),
              std::move(titleView).With(huxerui::Grow(1.0F)),
              std::move(actions),
          }.With(huxerui::Spacing(theme.spacing.small),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    header = std::move(header).With(huxerui::Frame{.min_height = kPageHeaderHeight});
    const float horizontal = fullWidthSections ? kSectionCardSpacing
        : (compact ? 4.0F : kDesktopPageHorizontalInset);
    if (fullWidthSections) {
        header = huxerui::View{header}.With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                compact ? horizontal : kDesktopPageHorizontalInset, 0.0F)));
        body = huxerui::ProvideEnvironment(SectionTabContentInsets{horizontal}, body);
    }
    // 一级/二级页共用内容层底色，保持与窗口外框的颜色区分，不套外卡。
    huxerui::View scaffold = huxerui::Column {
        std::move(header),
        std::move(body),
    }.With(huxerui::Padding(PageContentInsets(theme, compact,
                   fullWidthSections ? 0.0F : horizontal)),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(kPageTitlesInWindow || compact ? 0.0F : islands.island_radius),
           huxerui::ClipChildren(), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    if (backAction && !hideBack) {
        scaffold = std::move(scaffold).On<huxerui::ViewEvents::BackRequested>(
            std::move(backAction));
    }
    return scaffold;
}

[[huxerui::composable]] huxerui::View PillSearchField(
    huxerui::State<huxerui::TextEditingValue> value,
    huxerui::StringVariant placeholder, huxerui::View trailingAction) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto inputFocused = huxerui::UseState(false);
    const bool focused = kPageTitlesInWindow && !compact && inputFocused.Get();
    const bool animateFocus = !theme.motion.reduced_motion &&
        huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    const float height = kPageTitlesInWindow
        ? compact ? kDesktopTitleBarHeight : kDesktopTitleBarSearchHeight
        : 48.0F;
    huxerui::TextFieldStyle inputStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    inputStyle.border_width = 0.0F;
    inputStyle.focused_border_width = 0.0F;
    inputStyle.validation_border_width = 0.0F;
    inputStyle.focused_validation_border_width = 0.0F;
    inputStyle.standard.background = huxerui::Color::Transparent();
    inputStyle.standard.border = huxerui::Color::Transparent();
    inputStyle.standard.hovered_border = huxerui::Color::Transparent();
    inputStyle.standard.focused_border = huxerui::Color::Transparent();
    inputStyle.standard.disabled_border = huxerui::Color::Transparent();
    inputStyle.standard.minimum_height = height;
    inputStyle.padding = huxerui::EdgeInsets::Symmetric(0.0F, 0.0F);
    huxerui::ThemeDefinition inputTheme;
    inputTheme.Set(inputStyle);
    huxerui::View input = huxerui::Theme(
        std::move(inputTheme),
        huxerui::TextField(value.Get())
            .Placeholder(placeholder)
            .Variant(huxerui::TextFieldVariant::Standard)
            .OnChanged([value](const huxerui::TextEditingValue& next) {
                value = next;
            })
            .On<huxerui::ViewEvents::FocusChanged>([inputFocused](bool focused) {
                inputFocused = focused;
            })
            .With(huxerui::Grow(1.0F)));

    // 胶囊体内容：搜索图标、无框输入、按需清空，页面动作始终在最右侧。
    std::vector<huxerui::View> pillChildren{
        huxerui::Image(app::images::search)
            .Fit(huxerui::ImageFit::Contain)
            .Align(huxerui::HorizontalAlignment::Center,
                   huxerui::VerticalAlignment::Center)
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        std::move(input),
    };
    if (!value.Get().text.empty()) {
        pillChildren.push_back(
            huxerui::IconButton(app::images::close, Localized("清除搜索"))
                .With(huxerui::Tooltip(Localized("清除搜索")))
                .OnClick([value] { value = huxerui::TextEditingValue{}; }));
    }
    if (trailingAction) pillChildren.push_back(std::move(trailingAction));
    huxerui::View pillContent = huxerui::Row(std::move(pillChildren)).With(
           huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F));

    // 桌面宽屏搜索框上下留白，窄屏和手机保持原有高度。描边必须有：
    // 浅色主题里胶囊底（surface_container_highest ≈ #FDFDFD）与标题栏/页面
    // 岛底色（#FAFBFC / #EFF1F4）几乎同色，无描边时整个胶囊不可见。
    huxerui::View pill = huxerui::Row {
        std::move(pillContent),
    }.With(huxerui::Frame{.height = height},
           huxerui::Padding(huxerui::EdgeInsets{
               .right = 4.0F,
               .left = 14.0F,
           }),
           huxerui::Background(focused ? huxerui::Color::Transparent()
                                      : theme.colors.surface_container_highest),
           huxerui::Border(theme.colors.outline, 1.0F),
           huxerui::CornerRadius(height / 2.0F),
           huxerui::ClipChildren(),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F));

    pill = WithSearchFocusOutline(std::move(pill), focused, theme.colors.primary, animateFocus);
    return PageSearchFieldLayout(std::move(pill).Key("pill-search-field"), compact);
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content,
                                           bool outlined) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 二级岛：半透明水晶卡片感的 raised 表面 + 同心圆角。移动端设置页等
    // 场景不需要描边，由同层级的 raised 表面与页面底色自然区分。
    //
    // 表面/圆角/描边必须在**建树时**落在这个 Column 上：composable 返回的是 hcg
    // 生成的 Scope 包装节点，调用方在 `Card(...)` 的返回值上再补
    // `.With(Background/Border/CornerRadius)` 只会画在整棵子树背后，从圆角差里漏出
    // 一圈底色（首页「当前订阅」卡、订阅卡都踩过：深灰卡 + 蓝色圆角边）。需要另一种
    // 表面就再加一个建树型 composable（如 `SelectableTile`），不要事后装修。
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(islands.island_padding),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::Border(outlined ? islands.outline_soft
                                       : huxerui::Color::Transparent(),
                              outlined ? kCardOutlineWidth : 0.0F),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 通用「拖入或点击」文件选区（声明见 file_drop_zone.h）。FileDropTarget 只做
// 外部文件的整批元数据校验（扩展名/MIME，整批要么全收要么拒绝），读取与导入
// 仍由调用方决定；点击走 FilePicker——键盘、触摸与不支持拖入的宿主都依赖这个
// 等价入口（框架 file_drop.h 的契约要求）。悬停由 Entered/Exited 翻本地 State
// 驱动重组，物理松手在 Exited 之后才投递 Dropped。圆角/描边跟控件族
// （shapes.small + outline_soft），与按钮、下拉、菜单同族。
[[huxerui::composable]] huxerui::View FileDropZone(
    huxerui::ImageVariant icon, huxerui::StringVariant title,
    huxerui::State<std::string> picked_path, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    huxerui::FilePickerFilter filter, std::string key) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto hovering = huxerui::UseState(false);
    const bool hover = hovering.Get();
    const std::string pickedPath = picked_path.Get();
    std::string fileName = pickedPath;
    const std::size_t slash = pickedPath.find_last_of("/\\");
    if (slash != std::string::npos) fileName = pickedPath.substr(slash + 1);

    huxerui::FileDropOptions dropOptions;
    dropOptions.extensions = filter.extensions;
    dropOptions.allows_multiple = false;

    const auto open_picker = [tasks, picker, toast, filter, picked_path] {
        tasks.Launch([picker, toast, filter, picked_path]() -> huxerui::Task<void> {
            const auto picked = co_await PickProfileConfigFile(picker, filter, toast);
            if (!picked) co_return;
            if (const auto file = picked->AsFile()) picked_path = file->Path();
        });
    };

    const huxerui::Color captionColor = hover ? theme.colors.on_primary_container
                                              : theme.colors.on_surface_variant;
    return huxerui::Column {
        huxerui::Image(icon)
            .Tint(hover ? theme.colors.primary : theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 30.0F, .height = 30.0F}),
        huxerui::Text(fileName.empty() ? title : huxerui::StringVariant(fileName))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      hover ? theme.colors.on_primary_container
                                            : theme.colors.on_surface})
            .Align(huxerui::TextAlign::Center),
        pickedPath.empty()
            ? huxerui::View{huxerui::Text(huxerui::StringVariant(filter.name))
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    captionColor})
                                .Align(huxerui::TextAlign::Center)}
            : huxerui::View{huxerui::Text(pickedPath)
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    captionColor})
                                .Align(huxerui::TextAlign::Center)},
    }
        .With(huxerui::Spacing(6.0F),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Frame{.height = 128.0F},
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(20.0F, 12.0F)),
              huxerui::Background(hover ? theme.colors.primary_container
                                        : theme.colors.surface_container_high),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::Border(hover ? theme.colors.primary : islands.outline_soft,
                              hover ? kCardEmphasisOutlineWidth : kCardOutlineWidth),
              huxerui::ClipChildren(),
              huxerui::FileDropTarget::Accepts(std::move(dropOptions)),
              huxerui::PointerCursor(huxerui::PointerCursorKind::Hand))
        .On<huxerui::FileDropEvents::Entered>(
            [hovering](const huxerui::FileDropOffer&, const huxerui::FileDropEvent&) {
                hovering = true;
            })
        .On<huxerui::FileDropEvents::Exited>(
            [hovering](const huxerui::FileDropOffer&, const huxerui::FileDropEvent&) {
                hovering = false;
            })
        .On<huxerui::FileDropEvents::Dropped>(
            [picked_path](const std::vector<huxerui::FileReference>& files,
                          const huxerui::FileDropEvent&) {
                if (files.empty()) return;
                if (const auto file = files.front().AsFile()) picked_path = file->Path();
            })
        .On<huxerui::FileDropEvents::Failed>(
            [toast](const huxerui::IoError& error, const huxerui::FileDropEvent&) {
                toast.Show(LocalizedFormat("接收文件失败：{}", error.message));
            })
        .OnClick(open_picker)
        .With(huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(title)})
        .Key(std::move(key));
}

// 密集卡片（声明见 ui.h）：圆角阶梯的 dense 档（dense_radius 4pt），内边距
// 减半（island_padding/2）让高密度内容（日志行等）单行更紧凑；raised 表面
// 与卡片同层，靠圆角与留白密度区分。
[[huxerui::composable]] huxerui::View DenseCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(islands.island_padding * 0.5F),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.dense_radius),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 可选中卡片的形状常量见 ui.h（代理页节点卡为标准）。

SelectableTileColors ResolveSelectableTileColors(const huxerui::ThemeSpec& theme,
                                                 bool selected) {
    SelectableTileColors colors;
    if (selected) {
        // 选中态是「容器淡染底 + 主色描边」（描边由 SelectableTile 落笔）：
        // 实心 primary 铺满整卡过重，primary_container 保持表面层次，文字用
        // on_primary_container 常规对比，标题不再整段反色。
        colors.surface = theme.colors.primary_container;
        colors.fg = theme.colors.on_primary_container;
        colors.muted = theme.colors.on_primary_container;
        colors.faint = theme.colors.on_primary_container;
        colors.muted.alpha = 0.78F;
        colors.faint.alpha = 0.62F;
    } else {
        colors.surface = ResolveIslandTheme(theme).active;
        colors.fg = theme.colors.on_surface;
        colors.muted = theme.colors.on_surface_variant;
        colors.faint = theme.colors.outline;
    }
    return colors;
}

[[huxerui::composable]] huxerui::View SelectableTile(
    huxerui::View content, bool selected, std::string semantics_label,
    std::function<void()> on_click, SelectableTileInteraction interaction) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const SelectableTileColors colors =
        ResolveSelectableTileColors(theme, selected);
    const bool interactive = interaction != SelectableTileInteraction::Disabled &&
                             static_cast<bool>(on_click);
    // 表面/圆角/描边在建树时就落在 Column 上（不能在返回值外面补修饰符，原因见
    // Card 的注释）：未选中 islands.active，选中 primary_container 淡染底 +
    // primary 描边。
    huxerui::View tile =
        huxerui::Column {std::move(content)}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                  huxerui::Background(colors.surface),
                  huxerui::CornerRadius(kSelectableTileRadius),
                  huxerui::Border(selected ? theme.colors.primary
                                           : huxerui::Color::Transparent(),
                                  selected ? kCardSelectedOutlineWidth : 0.0F),
                  huxerui::ClipChildren(),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Semantics{.role = interactive ? huxerui::SemanticRole::Button
                                                           : huxerui::SemanticRole::Generic,
                                     .label = std::move(semantics_label)},
                  huxerui::Focusable(interactive),
                  huxerui::Enabled(interaction != SelectableTileInteraction::Disabled));
    if (interactive) {
        tile = std::move(tile).OnClick(std::move(on_click));
    }
    return tile;
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        kPageTitlesInWindow ? ResolveFluxShadows(theme).dialog
            : huxerui::Shadow{ThemeShadowColor(theme, 0.30F), {}, 28.0F, 2.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, kCardOutlineWidth),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}


// ---- 订阅列表：唯一来源（见 profiles_model.h）-------------------------------

void ProfilesModel::PublishCached() {
    auto& database = clashflux::persistence::persistence();
    Publish(database.listProfiles());
    if (database.ready()) loaded = true;
}

void DriveProfilesModel(huxerui::TaskScope tasks,
                        std::shared_ptr<ProfilesModel> model) {
    DriveProfileList(tasks, model,
        [] { return clashflux::persistence::persistence().profilesRevision(); },
        []() -> huxerui::Task<ProfileListSnapshot<db::Profile>> {
            co_return co_await RunOnTaskThread([] {
                ProfileListSnapshot<db::Profile> next;
                next.ready = clashflux::persistence::persistence().ready();
                next.profiles = clashflux::persistence::persistence().listProfiles();
                const auto root = cfg::profilesDir();
                for (const auto& profile : next.profiles) {
                    if (profile.type == "pptp" || profile.file.empty()) continue;
                    // Classification only needs a bounded prefix.
                    std::ifstream input(root / profile.file, std::ios::binary);
                    std::string prefix(20001, '\0');
                    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
                    prefix.resize(static_cast<std::size_t>(input.gcount()));
                    if (!prefix.empty() && singbox::DetectProfileFormat(prefix) == singbox::ProfileFormat::ClashYaml)
                        next.clashSources.push_back(profile.id);
                }
                return next;
            });
        });
}

// ---- 应用级设置：唯一读取点（见 settings_model.h）---------------------------

void DriveSettingsModel(huxerui::TaskScope tasks,
                        std::shared_ptr<SettingsModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        for (;;) {
            const std::uint64_t before = model->syncTick.Get();
            SettingsView next;
            auto& core = store::coreStore();
            next.ready = clashflux::persistence::persistence().ready();
            const std::string themeMode = core.setting("ui.theme_mode", "1");
            next.themeMode = themeMode == "0" ? 0 : themeMode == "2" ? 2 : 1;
            next.themeColor = core.setting("ui.theme_color", "blue");
            next.customThemeColors = core.setting("ui.custom_theme_colors", "");
            next.hiddenThemeColors = core.setting("ui.hidden_theme_colors", "");
            next.autoStart = core.setting("app.autostart", "false") == "true";
            next.autoRun = core.setting("app.auto_run", "false") == "true";
            next.trayEnabled = core.setting("tray.enabled", "true") == "true";
            next.startMinimized = next.trayEnabled &&
                core.setting("tray.start_minimized", "false") == "true";
            next.lightweightTray =
                core.setting("tray.lightweight", "false") == "true";
            next.mixedPort = core.setting("core.mixed_port", "7899");
            next.httpPort = core.setting("core.http_port", "");
            next.socksPort = core.setting("core.socks_port", "");
            next.envShell = core.setting("ui.env_shell", "");
            next.language = core.setting("ui.language", "system");
            // 都是内存读；State 按 operator== 去重，只有真的变了才通知订阅者。
            model->view = std::move(next);
            if (model->syncTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{1.0});
        }
    });
}

// ---- 原生连接状态：唯一来源（见 vpn_model.h）-------------------------------

void DriveVpnModel(huxerui::TaskScope tasks, std::shared_ptr<VpnModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        for (;;) {
            if (model->operation.Get().Busy()) {
                co_await huxerui::Delay(std::chrono::duration<double>{0.1});
                continue;
            }
            const std::uint64_t before = model->syncTick.Get();
            auto next = co_await RunOnTaskThread([] {
                return store::vpnStore().snapshot();
            });
            // 动作完成回调可能已发布更新的快照，不能用在途旧轮询覆盖它。
            if (!model->PublishPolled(before, std::move(next))) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{0.25});
        }
    });
}

// ---- 推送流变化 → UI 线程（见 stream_updates.h）-----------------------------

std::uint64_t SubscribeStreamUpdates(
    huxerui::TaskScope tasks,
    std::function<void(stream::StreamKind kind)> handler) {
    struct Consumer {
        std::function<void(stream::StreamKind)> handler;
        bool failureReported = false;
    };
    auto shared = std::make_shared<Consumer>(std::move(handler));
    return stream::addStreamUpdateObserver(
        [tasks, shared](stream::StreamKind kind) {
            // 推送线程：只 Post（scope 已关闭时 Post 被忽略），不在本线程碰 State。
            tasks.Post([shared, kind] {
                if (!stream::consumeStreamUpdate(kind, shared->handler) && !shared->failureReported) {
                    shared->failureReported = true;
                    stream::logApplication("warning", "runtime-health UI stream consumer failed; subsequent samples remain enabled");
                }
            });
        });
}

void UnsubscribeStreamUpdates(std::uint64_t id) {
    stream::removeStreamUpdateObserver(id);
}

} // namespace clashflux::ui
