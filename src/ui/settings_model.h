// settings_model.h — 应用级设置（KV）的唯一读取点。
//
// 为什么需要它：`store::coreStore().setting(...)` 读的是 persistence 的**内存
// 缓存**，而缓存在**首帧之后**的启动任务里才 hydrate。组合期直接
// `UseState(setting(...))` 只会拿到默认值，且之后没有任何东西再同步它——表现为
// 「关闭窗口时」高亮成默认的“每次询问」、开机自启/托盘开关显示错、首页保存过的
// 自定义布局在启动时丢失（主题当年被 AppRoot 显式校正过，其它项没有）。
//
// 现在所有“组合期要读的应用设置”都从这里取：驱动循环每拍读一次（便宜的内存
// 读），内容变化才发布（State 去重），并把 hydrate 是否完成（ready）一起发布，
// 需要“hydrate 后补一次”的消费者（首页布局）据此补读。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>

namespace clashflux::ui {

struct SettingsView {
    // persistence 是否已 hydrate：为 false 时下面各项都是默认值，消费者不要
    // 据此覆盖用户可见状态（例如不要用默认布局替换会话中的布局）。
    bool ready = false;
    int themeMode = 1;             // ui.theme_mode: 0 系统 / 1 深色 / 2 浅色
    bool autoStart = false;       // app.autostart
    bool autoRun = false;         // app.auto_run
    bool trayEnabled = true;      // tray.enabled
    bool startMinimized = false;  // tray.start_minimized
    int closeBehavior = 0;        // tray.close_behavior: 0 询问 / 1 退出 / 2 最小化
    std::string envShell;         // ui.env_shell（空 = 未保存，用探测值）
    std::string language = "system"; // ui.language: system / zh / en

    bool operator==(const SettingsView&) const = default;
};

struct SettingsModel {
    // 唯一订阅点。
    huxerui::State<SettingsView> view{SettingsView{}};
    // 外部来源（CLI/托盘/其他进程）改动后自增：驱动循环下一拍重读一次。
    // 注意它**不会**唤醒正在 Delay 的泵，只影响下一次读取——用户自己的点击
    // 走 Update() 写透，不要靠它来刷新界面。
    huxerui::State<std::uint64_t> syncTick{0};

    void RequestSync() { syncTick = syncTick.Get() + 1; }

    // 写透：设置项的内存缓存写完（setSetting 是同步写缓存）后，直接把同一个值
    // 发布进模型。UI 真值与缓存真值同一时刻成立，因此 KV 类设置不需要 pending、
    // 不需要乐观回落，也不需要等下一个泵节拍（见 AGENTS.md 的乐观控件约定）。
    template <class Change> void Update(Change&& change) {
        SettingsView next = view.Get();
        change(next);
        view = std::move(next);
    }
};

// 启动唯一的数据泵（组合期调用一次）。
void DriveSettingsModel(huxerui::TaskScope tasks,
                        std::shared_ptr<SettingsModel> model);

} // namespace clashflux::ui
