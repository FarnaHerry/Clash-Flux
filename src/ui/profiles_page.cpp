// profiles_page.cpp — 订阅页：矩形卡片网格（Flow 自动换行，Compact 视口退化为
// 整宽列表）。卡片交互：右上角刷新图标更新订阅；右键弹上下文菜单（使用/更新/
// 编辑信息/编辑规则/删除）；双击卡片切换启用订阅。
//
// 数据流：列表经 1s 泵从 profilesStore 重读；导入/更新/启用/删除/编辑保存都是
// 阻塞活（网络下载 / 内核重启），全部经 RunOnTaskThread。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.db;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

namespace clashflux::ui {
namespace {

// 卡片固定宽度（非 Compact 视口）。
constexpr float kCardWidth = 280.0F;

[[huxerui::composable]] huxerui::View ProfileCard(
    const db::Profile& profile, bool compact, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> reload) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto menu = huxerui::UseMenu();
    auto dialog = huxerui::UseDialog();
    auto editName = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editUrl = huxerui::UseState(huxerui::TextEditingValue{""});
    auto editYaml = huxerui::UseState(huxerui::TextEditingValue{""});
    const std::int64_t id = profile.id;

    auto action = [tasks, toast, reload](std::function<std::string()> job) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const std::string err = co_await RunOnTaskThread(std::move(job));
            if (!err.empty()) toast.Show(err);
            reload();
        });
    };

    // 编辑信息弹窗：名称 + 订阅链接（仅落库，URL 下次「更新」拉取时生效）。
    auto showEditInfo = [dialog, tasks, toast, editName, editUrl, id, profile] {
        editName = huxerui::TextEditingValue{profile.name};
        editUrl = huxerui::TextEditingValue{profile.url};
        dialog.Show(
            [tasks, toast, editName, editUrl,
             id](huxerui::DialogContext ctx) -> huxerui::View {
                return DialogCard(huxerui::Column {
                    huxerui::Text("编辑信息", huxerui::TextRole::Title),
                    huxerui::TextField(editName.Get())
                        .Label("名称")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([editName](const huxerui::TextEditingValue& v) {
                            editName = v;
                        }),
                    huxerui::TextField(editUrl.Get())
                        .Label("订阅链接（留空 = 本地导入）")
                        .Placeholder("https://...")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([editUrl](const huxerui::TextEditingValue& v) {
                            editUrl = v;
                        }),
                    huxerui::Row {
                        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                        huxerui::Button("保存").OnClick([=] {
                            ctx.Dismiss();
                            tasks.Launch([=]() -> huxerui::Task<void> {
                                const std::string name = editName.Get().text;
                                const std::string url = editUrl.Get().text;
                                const std::string err = co_await RunOnTaskThread(
                                    [id, name, url] {
                                        auto& ps = store::profilesStore();
                                        if (!ps.updateInfo(id, name, url))
                                            return ps.lastError();
                                        return std::string{};
                                    });
                                if (!err.empty()) {
                                    toast.Show(err);
                                } else {
                                    toast.Show("已保存");
                                }
                            });
                        }),
                    }.With(huxerui::MainAlign(
                               huxerui::MainAxisAlignment::SpaceBetween)),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::Frame{.width = 360.0F},
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    // 编辑规则弹窗：直接编辑订阅 YAML 原文；保存后若该订阅启用中将重启内核。
    auto showEditYaml = [dialog, tasks, toast, editYaml, id, profile] {
        editYaml = huxerui::TextEditingValue{store::profilesStore().yamlOf(id)};
        dialog.Show(
            [tasks, toast, editYaml, id,
             name = profile.name](huxerui::DialogContext ctx) -> huxerui::View {
                return DialogCard(huxerui::Column {
                    huxerui::Text("编辑规则 — " + name, huxerui::TextRole::Title),
                    huxerui::Text("直接编辑订阅 YAML；保存后若该订阅启用中，"
                                  "内核将重启使改动生效。")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            huxerui::UseTheme().colors.on_surface_variant}),
                    huxerui::TextField(editYaml.Get())
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .LineLimits(huxerui::TextFieldLineLimits::MultiLine(14, 22))
                        .OnChanged([editYaml](const huxerui::TextEditingValue& v) {
                            editYaml = v;
                        }),
                    huxerui::Row {
                        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                        huxerui::Button("保存").OnClick([=] {
                            ctx.Dismiss();
                            tasks.Launch([=]() -> huxerui::Task<void> {
                                const std::string content = editYaml.Get().text;
                                const std::string err = co_await RunOnTaskThread(
                                    [id, content] {
                                        auto& ps = store::profilesStore();
                                        if (!ps.saveYaml(id, content))
                                            return ps.lastError();
                                        return std::string{};
                                    });
                                if (!err.empty()) {
                                    toast.Show(err);
                                } else {
                                    toast.Show("已保存");
                                }
                            });
                        }),
                    }.With(huxerui::MainAlign(
                               huxerui::MainAxisAlignment::SpaceBetween)),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::Frame{.width = 640.0F},
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    // 右键菜单：使用（未启用时）/ 更新 / 编辑信息 / 编辑规则 / 删除。
    auto showMenu = [menu, action, showEditInfo, showEditYaml, id,
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
        entries.push_back(huxerui::MenuSection{});
        entries.push_back(huxerui::MenuItem("编辑信息", showEditInfo));
        entries.push_back(huxerui::MenuItem("编辑规则", showEditYaml));
        entries.push_back(huxerui::MenuSection{});
        entries.push_back(huxerui::MenuItem("删除", [action, id] {
            action([id]() -> std::string {
                auto& ps = store::profilesStore();
                ps.remove(id);
                return ps.lastError();
            });
        }));
        menu.ShowAt(pos, std::move(entries));
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
            huxerui::Text(profile.name).Style(huxerui::TextStyle{
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
        huxerui::Text(profile.url.empty() ? "本地导入" : profile.url)
            .Style(huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip),
                                      theme.colors.on_surface_variant}),
        huxerui::Text(profile.error.empty()
                          ? (profile.updatedAt > 0
                                 ? "更新于 " + formatTime(profile.updatedAt)
                                 : "未拉取")
                          : "错误：" + profile.error)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                profile.error.empty() ? theme.colors.on_surface_variant
                                      : theme.colors.error}),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));

    // 矩形卡：非 Compact 定宽（Flow 网格换行），Compact 由父列 Stretch 整宽。
    if (!compact) {
        card = std::move(card).With(huxerui::Frame{.width = kCardWidth});
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
        .On<huxerui::ViewEvents::ContextMenuRequested>(showMenu)
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
    auto profiles = huxerui::UseState<std::vector<db::Profile>>({});
    auto newName = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newUrl = huxerui::UseState(huxerui::TextEditingValue{""});
    auto importing = huxerui::UseState(false);

    // 列表泵：1s 一拍重读（CRUD 后也会手动触发）。
    huxerui::Lifecycle(
        [tasks, profiles] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
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
            // 列表由 1s 泵刷新；这里仅给一拍推迟，保证任务内 State 写不与
            // 点击路径叠加。
        });
    };

    // 新建订阅弹窗：名称可选 + 订阅链接；导入是网络下载（阻塞），走任务线程，
    // 成功才关弹窗。打开前清空上一轮的输入。
    auto showCreateDialog = [dialog, tasks, toast, newName, newUrl, importing] {
        newName = huxerui::TextEditingValue{""};
        newUrl = huxerui::TextEditingValue{""};
        dialog.Show(
            [tasks, toast, newName, newUrl,
             importing](huxerui::DialogContext ctx) -> huxerui::View {
                return DialogCard(huxerui::Column {
                    huxerui::Text("新建订阅", huxerui::TextRole::Title),
                    huxerui::TextField(newName.Get())
                        .Label("名称（可选）")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([newName](const huxerui::TextEditingValue& v) {
                            newName = v;
                        }),
                    huxerui::TextField(newUrl.Get())
                        .Label("订阅链接")
                        .Placeholder("https://...")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([newUrl](const huxerui::TextEditingValue& v) {
                            newUrl = v;
                        }),
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
                                      if (url.empty()) {
                                          toast.Show("订阅链接不能为空");
                                          return;
                                      }
                                      importing = true;
                                      tasks.Launch([=]() -> huxerui::Task<void> {
                                          const std::string name =
                                              newName.Get().text;
                                          const auto [id, err] =
                                              co_await RunOnTaskThread(
                                                  [name, url] {
                                                      auto& ps =
                                                          store::profilesStore();
                                                      const std::int64_t id =
                                                          ps.importUrl(name, url);
                                                      return std::pair{
                                                          id, ps.lastError()};
                                                  });
                                          importing = false;
                                          if (id == 0) {
                                              toast.Show(err.empty() ? "导入失败"
                                                                     : err);
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
                }.With(huxerui::Spacing(12.0F),
                       huxerui::Frame{.width = 360.0F},
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    std::vector<huxerui::View> cards;
    for (const auto& p : profiles.Get()) {
        cards.push_back(ProfileCard(p, compact, tasks, toast, reload).Key(p.id));
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
