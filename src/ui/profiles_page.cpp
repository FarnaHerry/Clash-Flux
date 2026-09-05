// profiles_page.cpp — 订阅页：一订阅一卡片的卡片布局 + 右上角「新建订阅」
// 弹窗导入（名称可选 + 订阅链接）。
//
// 数据流：列表经 1s 泵从 profilesStore 重读（CRUD 后立即重读）；导入/更新/
// 启用/删除都是阻塞活（网络下载 / 内核重启），全部经 RunOnTaskThread。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.db;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

namespace clashflux::ui {
namespace {

[[huxerui::composable]] huxerui::View ProfileCard(
    const db::Profile& profile, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> reload) {
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

    return Card(huxerui::Column {
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
            profile.selected
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Button("启用").OnClick([action, id] {
                      action([id]() -> std::string {
                          auto& ps = store::profilesStore();
                          if (!ps.activate(id)) return ps.lastError();
                          return "";
                      });
                  })},
            huxerui::Button("更新").OnClick([action, id] {
                action([id]() -> std::string {
                    auto& ps = store::profilesStore();
                    if (!ps.refresh(id)) return ps.lastError();
                    return "";
                });
            }),
            huxerui::Button("删除").OnClick([action, id] {
                action([id]() -> std::string {
                    auto& ps = store::profilesStore();
                    ps.remove(id);
                    return ps.lastError();
                });
            }),
        }.With(huxerui::Spacing(8.0F),
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
}

} // namespace

[[huxerui::composable]] huxerui::View ProfilesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
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
        cards.push_back(ProfileCard(p, tasks, toast, reload).Key(p.id));
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
                                huxerui::Column(std::move(cards))
                                    .With(huxerui::Spacing(10.0F),
                                          huxerui::CrossAlign(
                                              huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F))});
}

} // namespace clashflux::ui
