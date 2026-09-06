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
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.db;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

namespace clashflux::ui {
namespace {

// 卡片统一尺寸：定宽（Flow 网格换行）+ 定高（内容单行截断，ClipChildren
// 兜底）；Compact 视口整宽（高度仍统一）。
constexpr float kCardWidth = 280.0F;
constexpr float kCardHeight = 156.0F;

// 弹窗表单区滚动视口高度：字段多（类型/描述/超时/间隔/四个开关），限高防
// 小窗溢出。
constexpr float kDialogFormHeight = 340.0F;

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
        ToggleRow("使用系统代理更新", "经环境变量代理拉取订阅", false, sysProxy),
        ToggleRow("使用内核代理更新", "经本应用内核混合端口拉取（内核需运行）",
                  false, coreProxy),
        ToggleRow("允许无效证书（危险）", "跳过 HTTPS 证书校验，仅用于可信来源",
                  true, invalidCert),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 单张订阅卡：纯视图（零弹窗 State；菜单句柄/任务域由页面下发），弹窗经
// openXxx(id) 回调到页面级懒加载打开。
[[huxerui::composable]] huxerui::View ProfileCard(
    const db::Profile& profile, bool compact, huxerui::MenuHandle menu,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::function<void()> reload,
    const std::function<void(std::int64_t)>& openEditInfo,
    const std::function<void(std::int64_t)>& openEditRules,
    const std::function<void(std::int64_t)>& openEditFile,
    const std::function<void(std::int64_t)>& openQr) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const std::int64_t id = profile.id;

    auto action = [tasks, toast, reload](std::function<std::string()> job) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const std::string err = co_await RunOnTaskThread(std::move(job));
            if (!err.empty()) toast.Show(err);
            reload();
        });
    };

    // 右上角刷新图标：裸 Image + Tint 着色（IconButton 不着色矢量资源；
    // 同 apitab 标题栏齿轮配方），自绘 24pt 正方形热区。
    huxerui::View refreshButton =
        huxerui::Row {
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
            huxerui::Spacer(),
            std::move(refreshButton),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(truncateOneLine(
                          profile.url.empty() ? "本地导入" : profile.url, 40))
            .Style(huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip),
                                      theme.colors.on_surface_variant}),
        huxerui::Text(truncateOneLine(
                          profile.description.empty() ? "—"
                                                      : profile.description,
                          44))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                profile.description.empty()
                    ? theme.colors.outline
                    : theme.colors.on_surface_variant}),
        huxerui::Row {
            huxerui::Text(profile.type == "local" ? "本地" : "远程")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            profile.autoUpdate && profile.intervalMins > 0
                ? huxerui::View{huxerui::Text(std::format("自动 {} 分钟",
                                                          profile.intervalMins))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(
                                            font_size::kCaption),
                                        theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
            huxerui::Spacer(),
            huxerui::Text(truncateOneLine(
                              profile.error.empty()
                                  ? (profile.updatedAt > 0
                                         ? "更新于 " + formatTime(profile.updatedAt)
                                         : "未拉取")
                                  : "错误：" + profile.error,
                              44))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    profile.error.empty() ? theme.colors.on_surface_variant
                                          : theme.colors.error}),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        // 流量行 + 进度条（订阅响应头 subscription-userinfo；total 未知不显示）。
        profile.totalBytes > 0
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
        .With(huxerui::MultiTapGesture{.count = 2})
        // 双击切换启用订阅（未启用时）。
        .On<huxerui::MultiTapEvents::Recognized>(
            [action, id, selected = profile.selected](const huxerui::MultiTapEvent&) {
                if (selected) return;
                action([id]() -> std::string {
                    auto& ps = store::profilesStore();
                    if (!ps.activate(id)) return ps.lastError();
                    return "";
                });
            })
        // 右键上下文菜单（跟随点击位置弹出）。
        .On<huxerui::ViewEvents::ContextMenuRequested>(
            [menu, action, openEditInfo, openEditRules, openEditFile, openQr,
             id, homepage = profile.homepage, url = profile.url,
             selected = profile.selected](huxerui::Point pos) {
                std::vector<huxerui::MenuEntry> entries;
                if (!selected) {
                    entries.push_back(huxerui::MenuItem("使用", [action, id] {
                        action([id]() -> std::string {
                            auto& ps = store::profilesStore();
                            if (!ps.activate(id)) return ps.lastError();
                            return "";
                        });
                    }));
                }
                entries.push_back(huxerui::MenuItem("更新", [action, id] {
                    action([id]() -> std::string {
                        auto& ps = store::profilesStore();
                        if (!ps.refresh(id)) return ps.lastError();
                        return "";
                    });
                }));
                if (!homepage.empty()) {
                    entries.push_back(huxerui::MenuItem("首页",
                                                        [action, homepage] {
                        action([homepage]() -> std::string {
                            core::openInBrowser(homepage);
                            return "";
                        });
                    }));
                }
                if (!url.empty()) {
                    entries.push_back(huxerui::MenuItem("分享二维码",
                                                        [openQr, id] {
                        openQr(id);
                    }));
                }
                entries.push_back(huxerui::MenuSection{});
                entries.push_back(huxerui::MenuItem("编辑信息", [openEditInfo, id] {
                    openEditInfo(id);
                }));
                entries.push_back(huxerui::MenuItem("编辑规则",
                                                    [openEditRules, id] {
                    openEditRules(id);
                }));
                entries.push_back(huxerui::MenuItem("编辑文件",
                                                    [openEditFile, id] {
                    openEditFile(id);
                }));
                entries.push_back(huxerui::MenuSection{});
                entries.push_back(huxerui::MenuItem("删除", [action, id] {
                    action([id]() -> std::string {
                        auto& ps = store::profilesStore();
                        ps.remove(id);
                        return ps.lastError();
                    });
                }));
                menu.ShowAt(pos, std::move(entries));
            })
        .Key(id);
}

} // namespace

[[huxerui::composable]] huxerui::View ProfilesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto menu = huxerui::UseMenu();
    auto clipboard = huxerui::UseService<huxerui::Clipboard>();
    auto profiles = huxerui::UseState<std::vector<db::Profile>>({});

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
    auto pickedPath = huxerui::UseState<std::string>("");

    // ---- 编辑订阅弹窗（页面级一份；卡片按 id 打开，避免 N 卡 × N 状态）----
    auto editName = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editUrl = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editDesc = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editTimeout = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editInterval = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editAuto = huxerui::UseState(false);
    auto editSys = huxerui::UseState(false);
    auto editCore = huxerui::UseState(false);
    auto editCert = huxerui::UseState(false);

    // ---- 编辑规则弹窗（工作 YAML + 规则行 + 输入行 + 脏标记）----
    auto editYamlText = huxerui::UseState<std::string>("");
    auto editRules = huxerui::UseState<std::vector<std::string>>({});
    auto editRuleInput = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editRulesDirty = huxerui::UseState(false);

    // ---- 编辑文件弹窗（懒加载 + 非受控大文本：shared_ptr 原地写，逐键零重组）----
    auto fileContent = huxerui::UseState(std::make_shared<std::string>());
    auto fileLoading = huxerui::UseState(false);

    // 列表泵：2s 一拍重读（State 相等时短路，无重组；CRUD 后手动 reload）。
    huxerui::Lifecycle(
        [tasks, profiles] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{2.0}, [=] {
                    profiles = store::profilesStore().list();
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
        for (const auto& p : profiles.Get()) {
            if (p.id == id) return p;
        }
        return std::nullopt;
    };

    // ---- 编辑信息（名称/URL/选项表单；仅落库，下次更新生效）----
    auto showEditInfo = [dialog, tasks, toast, profiles, findProfile,
                         editName, editUrl, editDesc, editTimeout,
                         editInterval, editAuto, editSys, editCore, editCert,
                         textColor = theme.colors.on_surface,
                         hintColor = theme.colors.on_surface_variant](
                            std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile) co_return;
            editName = huxerui::TextEditingValue{profile->name};
            editUrl = huxerui::TextEditingValue{profile->url};
            editDesc = huxerui::TextEditingValue{profile->description};
            editTimeout =
                huxerui::TextEditingValue{std::format("{}", profile->timeoutSecs)};
            editInterval =
                huxerui::TextEditingValue{std::format("{}", profile->intervalMins)};
            editAuto = profile->autoUpdate;
            editSys = profile->useSystemProxy;
            editCore = profile->useCoreProxy;
            editCert = profile->allowInvalidCert;
            dialog.Show(
                [tasks, toast, editName, editUrl, editDesc, editTimeout,
                 editInterval, editAuto, editSys, editCore, editCert, id,
                 textColor, hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑订阅", huxerui::TextRole::Title),
                        huxerui::ScrollView(huxerui::Column {
                            huxerui::TextField(editName.Get())
                                .Label("名称")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editName](const huxerui::TextEditingValue& v) {
                                        editName = v;
                                    }),
                            huxerui::TextField(editUrl.Get())
                                .Label("订阅链接（留空 = 本地导入）")
                                .Placeholder("https://...")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editUrl](const huxerui::TextEditingValue& v) {
                                        editUrl = v;
                                    }),
                            ProfileOptionsForm(editDesc, editTimeout,
                                               editInterval, editAuto, editSys,
                                               editCore, editCert),
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
                                ctx.Dismiss();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    db::Profile fields;
                                    fields.name = editName.Get().text;
                                    fields.url = editUrl.Get().text;
                                    fields.description = editDesc.Get().text;
                                    fields.timeoutSecs =
                                        parseNumber(editTimeout.Get(), 60);
                                    fields.intervalMins =
                                        parseNumber(editInterval.Get(), 0);
                                    fields.autoUpdate = editAuto.Get();
                                    fields.useSystemProxy = editSys.Get();
                                    fields.useCoreProxy = editCore.Get();
                                    fields.allowInvalidCert = editCert.Get();
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
            editRules = store::parseRules(yaml);
            editRuleInput = huxerui::TextEditingValue{""};
            editRulesDirty = false;
            dialog.Show(
                [tasks, toast, editYamlText, editRules, editRuleInput,
                 editRulesDirty, id, textColor, hintColor](
                    huxerui::DialogContext ctx) -> huxerui::View {
                    // 规则行（动态列表：键用行号；行内无状态）。
                    std::vector<huxerui::View> rows;
                    const std::vector<std::string>& rules = editRules.Get();
                    rows.reserve(rules.size());
                    for (std::size_t i = 0; i < rules.size(); ++i) {
                        rows.push_back(
                            huxerui::Row {
                                huxerui::Text(std::format("{}", i + 1))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(
                                            font_size::kCaption),
                                        hintColor})
                                    .With(huxerui::Frame{.width = 32.0F}),
                                huxerui::Text(truncateOneLine(rules[i], 72))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::Monospace(
                                            font_size::kMonoBody),
                                        textColor}),
                            }
                                .With(huxerui::Spacing(6.0F),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Center))
                                .Key(std::to_string(i)));
                    }
                    auto addRule = [=](bool prepend) {
                        const std::string text = editRuleInput.Get().text;
                        if (text.empty()) return;
                        const std::string yaml = store::insertRule(
                            editYamlText.Get(), text, prepend);
                        editYamlText = yaml;
                        editRules = store::parseRules(yaml);
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
                        huxerui::ScrollView(
                                rows.empty()
                                    ? huxerui::View{
                                          huxerui::Text("订阅没有 rules 规则")
                                              .Style(huxerui::TextStyle{
                                                  huxerui::Font::System(
                                                      font_size::kCaption),
                                                  hintColor})}
                                          .With(huxerui::Padding(12.0F))
                                    : huxerui::View{
                                          huxerui::Column(std::move(rows))
                                              .With(huxerui::Spacing(6.0F))})
                            .With(huxerui::Frame{.height = 300.0F}),
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
    // 类型（远程/本地）+ 名称 + 选项表单；导入是网络下载（阻塞），走任务
    // 线程，成功才关弹窗。打开前清空上一轮的输入。
    auto showCreateDialog = [dialog, tasks, toast, picker, newName, newUrl,
                             newTypeIdx, newDesc, newTimeout, newInterval,
                             newAuto, newSys, newCore, newCert, pickedPath,
                             importing, hintColor = theme.colors.on_surface_variant] {
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
        pickedPath = "";
        dialog.Show(
            [tasks, toast, picker, newName, newUrl, newTypeIdx, newDesc,
             newTimeout, newInterval, newAuto, newSys, newCore, newCert,
             pickedPath, importing, hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                const bool remote = newTypeIdx.Get() == 0;
                return DialogCard(huxerui::Column {
                    huxerui::Text("新建订阅", huxerui::TextRole::Title),
                    huxerui::ScrollView(huxerui::Column {
                        huxerui::SegmentedButton(
                            std::vector<huxerui::StringVariant>{"远程订阅",
                                                                "本地文件"},
                            newTypeIdx.Get())
                            .OnChanged([newTypeIdx](std::size_t idx) {
                                newTypeIdx = idx;
                            }),
                        remote
                            ? huxerui::View{huxerui::TextField(newUrl.Get())
                                                .Label("订阅链接")
                                                .Placeholder("https://...")
                                                .Variant(huxerui::
                                                             TextFieldVariant::
                                                                 Outlined)
                                                .OnChanged(
                                                    [newUrl](const huxerui::
                                                                 TextEditingValue&
                                                                     v) {
                                                        newUrl = v;
                                                    })}
                            : huxerui::View{huxerui::Row {
                                  huxerui::Button("选择文件")
                                      .OnClick([tasks, picker, pickedPath] {
                                          tasks.Launch(
                                              [=]() -> huxerui::Task<void> {
                                                  const auto picked =
                                                      co_await picker->
                                                          OpenFileAsync(
                                                              huxerui::
                                                                  FilePickerFilter{
                                                                      .name =
                                                                          "YAML 订阅",
                                                                      .extensions =
                                                                          {"yaml",
                                                                           "yml"}});
                                                  if (!picked) co_return;
                                                  if (const auto f =
                                                          picked->AsFile()) {
                                                      pickedPath = f->Path();
                                                  } else {
                                                      pickedPath = "";
                                                  }
                                              });
                                      }),
                                  huxerui::Text(
                                      pickedPath.Get().empty()
                                          ? "未选择文件"
                                          : pickedPath.Get())
                                      .Style(huxerui::TextStyle{
                                          huxerui::Font::Monospace(
                                              font_size::kChip),
                                          hintColor}),
                              }
                                  .With(huxerui::Spacing(8.0F),
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::
                                                Center))},
                        huxerui::TextField(newName.Get())
                            .Label("名称（可选）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([newName](const huxerui::TextEditingValue& v) {
                                newName = v;
                            }),
                        ProfileOptionsForm(newDesc, newTimeout, newInterval,
                                           newAuto, newSys, newCore, newCert),
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
                                      if (!remote && pickedPath.Get().empty()) {
                                          toast.Show("请先选择订阅文件");
                                          return;
                                      }
                                      importing = true;
                                      tasks.Launch([=]() -> huxerui::Task<void> {
                                          db::Profile options;
                                          options.description =
                                              newDesc.Get().text;
                                          options.timeoutSecs =
                                              parseNumber(newTimeout.Get(), 60);
                                          options.intervalMins =
                                              parseNumber(newInterval.Get(), 0);
                                          options.autoUpdate = newAuto.Get();
                                          options.useSystemProxy = newSys.Get();
                                          options.useCoreProxy = newCore.Get();
                                          options.allowInvalidCert =
                                              newCert.Get();
                                          const std::string name =
                                              newName.Get().text;
                                          const auto [rid, err] = co_await
                                              RunOnTaskThread(
                                                  [remote, name, url, options,
                                                   picked = pickedPath.Get()] {
                                                      auto& ps =
                                                          store::profilesStore();
                                                      const std::int64_t nid =
                                                          remote
                                                              ? ps.importUrl(
                                                                    name, url,
                                                                    options)
                                                              : ps.importFile(
                                                                    name,
                                                                    picked,
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

    std::vector<huxerui::View> cards;
    for (const auto& p : profiles.Get()) {
        cards.push_back(ProfileCard(p, compact, menu, tasks, toast, reload,
                                    showEditInfo, showEditRules, showEditFile,
                                    showQr)
                            .Key(p.id));
    }

    return PageScaffold(
        "订阅",
        huxerui::Row {
            huxerui::Button("新建订阅").OnClick(
                [tasks, showCreateDialog] {
                    // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        showCreateDialog();
                    });
                }),
        },
        cards.empty()
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
            : huxerui::View{huxerui::ScrollView(
                                compact
                                    ? huxerui::View{huxerui::Column(std::move(cards))
                                                        .With(huxerui::Spacing(10.0F),
                                                              huxerui::CrossAlign(
                                                                  huxerui::CrossAxisAlignment::Stretch))}
                                    : huxerui::View{huxerui::Flow(std::move(cards))
                                                        .With(huxerui::Spacing(
                                                            theme.spacing.medium))})
                                .With(huxerui::Grow(1.0F))});
}

} // namespace clashflux::ui
