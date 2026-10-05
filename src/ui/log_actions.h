#pragma once

#include <functional>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "action_menu.h"

namespace clashflux::ui {

inline const std::vector<std::string> kLogLevelNames{"全部", "信息", "警告", "错误", "调试"};
inline const std::array<huxerui::ImageResource, 5> kLogLevelIcons{
    huxerui::ImageResource{"app", "images/logs"},
    huxerui::ImageResource{"app", "images/info"},
    huxerui::ImageResource{"app", "images/warning"},
    huxerui::ImageResource{"app", "images/error"},
    huxerui::ImageResource{"app", "images/terminal"}};

struct LogEntry {
    std::string text;
    int level = 1;
};

// 导出与可见列表共用级别过滤，按原始顺序保留时间戳与文本。
inline std::string ExportLogText(const std::vector<LogEntry>& entries, std::size_t filter) {
    std::string text;
    for (const auto& entry : entries) {
        if (filter != 0 && entry.level != static_cast<int>(filter)) continue;
        text += entry.text;
        text += '\n';
    }
    return text;
}

// 每个弹出面板有独立 Scope，子菜单用原菜单项的稳定几何作锚点。
// 子面板不拦截父面板区域，鼠标可以回到父项切换，离开父项不立即关闭。
inline huxerui::View LogMenuPanel(huxerui::PopupContext context, std::size_t selectedLevel,
    std::function<void(std::size_t)> selectLevel, std::function<void()> copy,
    std::function<void()> exportFile, std::function<void()> clear, bool canExportFile) {
    const auto levelPopup = huxerui::UsePopup();
    const auto exportPopup = huxerui::UsePopup();
    auto child = huxerui::UseState<huxerui::LayerId>(0);
    auto opened = huxerui::UseState(0);
    const auto closeChild = [levelPopup, exportPopup, child, opened] {
        levelPopup.Dismiss(child.Get());
        exportPopup.Dismiss(child.Get());
        child = 0;
        opened = 0;
    };
    const auto dismiss = [closeChild, context] { closeChild(); context.Dismiss(); };
    huxerui::Lifecycle([levelPopup, exportPopup, child] {
        return [levelPopup, exportPopup, child] {
            levelPopup.Dismiss(child.Get());
            exportPopup.Dismiss(child.Get());
        };
    }, 0);
    huxerui::PopupOptions options{
        .placement = {huxerui::AnchorSide::Right, huxerui::AnchorAlignment::Start},
        .gap = 2.0F,
        .dismiss_on_outside_press = false,
        .on_dismiss_request = dismiss,
    };
    const auto openLevels = [=] {
        if (opened.Get() == 1) return;
        closeChild();
        child = levelPopup.Show([=] {
            return huxerui::Scope([=] {
                std::vector<huxerui::View> items;
                for (std::size_t index = 0; index < kLogLevelNames.size(); ++index) {
                    items.push_back(ActionMenuItemView(Localized(kLogLevelNames[index]),
                        [=] { dismiss(); selectLevel(index); },
                        index == selectedLevel ? "✓" : "", index == selectedLevel,
                        true, false, kLogLevelIcons[index]));
                }
                return ActionMenuSurface(std::move(items));
            });
        }, options);
        opened = 1;
    };
    const auto openExport = [=] {
        if (opened.Get() == 2) return;
        closeChild();
        child = exportPopup.Show([=] {
            return huxerui::Scope([=] {
                return ActionMenuSurface({
                    ActionMenuItemView(Localized("剪贴板"), [=] { dismiss(); copy(); },
                        "", {}, true, false, huxerui::ImageResource{"app", "images/copy"}),
                    ActionMenuItemView(Localized("文件"), [=] { dismiss(); exportFile(); },
                        "", {}, canExportFile, false, huxerui::ImageResource{"app", "images/save"}),
                });
            });
        }, options);
        opened = 2;
    };
    return ActionMenuSurface({
        ActionMenuItemView(Localized("级别"), openLevels, "›", {}, true, false,
            huxerui::ImageResource{"app", "images/logs"})
            .With(levelPopup.Anchor())
            .On<huxerui::ViewEvents::Hover>([openLevels](const huxerui::HoverEvent& event) {
                if (event.type == huxerui::HoverEventType::Enter) openLevels();
            }),
        ActionMenuItemView(Localized("导出"), openExport, "›", {}, true, false,
            huxerui::ImageResource{"app", "images/save"})
            .With(exportPopup.Anchor())
            .On<huxerui::ViewEvents::Hover>([openExport](const huxerui::HoverEvent& event) {
                if (event.type == huxerui::HoverEventType::Enter) openExport();
            }),
        ActionMenuItemView(Localized("清空"), [=] { dismiss(); clear(); },
            "", {}, true, true, huxerui::ImageResource{"app", "images/clear_all"})
            .On<huxerui::ViewEvents::Hover>([closeChild](const huxerui::HoverEvent& event) {
                if (event.type == huxerui::HoverEventType::Enter) closeChild();
            }),
    });
}

inline huxerui::View LogActionsMenu(
    std::size_t selectedLevel, std::function<void(std::size_t)> selectLevel,
    std::function<void()> copy, std::function<void()> exportFile,
    std::function<void()> clear, bool canExportFile = true) {
    auto popup = huxerui::UsePopup();
    return huxerui::IconButton(huxerui::ImageResource{"app", "images/more_vertical"}, Localized("日志操作"))
        .With(popup.Anchor(), huxerui::Tooltip(Localized("日志操作")))
        .OnClick([=] {
            popup.Show([=](huxerui::PopupContext context) {
                return huxerui::Scope([=] {
                    return LogMenuPanel(context, selectedLevel, selectLevel, copy, exportFile, clear, canExportFile);
                });
            });
        }).Key("log-actions-menu");
}

} // namespace clashflux::ui
