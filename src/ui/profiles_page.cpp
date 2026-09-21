// profiles_page.cpp — 订阅页：矩形卡片网格（定高、宽度由网格轨道均分铺满，
// 列数按窗口宽度自适应；内容单行 UTF-8 截断；Compact 视口退化为整宽列表）。
// 卡片交互：右上角刷新图标更新
// 订阅；桌面右键、Compact 的“更多”按钮共用上下文菜单（使用/更新/首页/
// 分享二维码/编辑信息/编辑规则/编辑文件/删除）；点击卡片切换启用订阅。
//
// 订阅选项（类型/描述/HTTP 超时/更新间隔/自动更新；桌面额外提供代理/证书开关）
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

#include "profiles_page_shared.h"

namespace clashflux::ui {

[[huxerui::composable]] huxerui::View ProfilesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 平台栈下载通道（Android 订阅导入/刷新用）：Runtime 各平台都装了该服务。
    auto http = huxerui::UseService<huxerui::HttpClient>();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto profiles = huxerui::UseStateList<db::Profile>();
    auto pptpStates = huxerui::UseStateList<store::PptpState>();
    auto openVpnStates = huxerui::UseStateList<store::OpenVpnState>();
    auto connectionSelection = huxerui::UseStateList<std::int64_t>();
    auto optimisticSelected =
        huxerui::UseState<std::optional<std::int64_t>>(std::nullopt);

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
    auto createPageOpen = huxerui::UseState(false);

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
    auto filePageId = huxerui::UseState<std::int64_t>(0);
    auto rulesPageId = huxerui::UseState<std::int64_t>(0);

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
    const auto buildContent = [&]() -> huxerui::View {
#include "ui/profiles_page_actions.inc"
#include "ui/profiles_page_layout.inc"
    };
    return buildContent();
}

} // namespace clashflux::ui
