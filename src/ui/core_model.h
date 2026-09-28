// core_model.h — 内核与接管状态的唯一来源（跨页面共享的 application service）。
//
// 此前首页三张桌面卡片各挂一条 0.5s 轮询、设置页 1s、内核设置段 1s、托盘 0.5s
// 各挂一条，全部直接读 store::coreStore()；其中托盘那条还要顺带跑
// checkAlive()（服务托管形态下是一次 HTTP /version）和 systemProxyActive()
// （GNOME 下是 3 次 popen gsettings），等于每秒数次子进程/网络往返，只为刷新
// 几个显示用的布尔值。
//
// 现在只有一个驱动循环（common.cpp 的 DriveCoreModel，1s 一拍；昂贵探测 ——
// 系统代理实际状态 —— 只每 15 拍或显式请求时做一次），把结果写进这里的 State。
// 各页面在组合期读 view 即可：读即订阅，内容变化（operator== 去重）才重组。
//
// 注意：CoreView 含 store::CoreSnapshot，本头文件必须在
// `import clashflux.store.core;` 之后包含（头文件不做模块导入，与
// profiles_cache.h/proxies_model.h 同一约定）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>

#include "ui.h"

namespace clashflux::ui {

// 内核 + 接管状态的显示模型。store::CoreSnapshot 已带 operator== 且包含
// state/version/lastError/mode/mixedPort/allowLan/logLevel/tunEnabled，
// 这里只补 UI 需要、但不在内核快照里的几项设置与探测结果。
struct CoreView {
    store::CoreSnapshot core;
    bool systemProxyIntent = false;   // 用户意图（proxy.system_enabled）
    bool systemProxyActive = false;   // 系统代理是否确实指向本应用（昂贵探测）
    bool ipv6Enabled = false;         // core.ipv6_enabled
    // core.allow_lan 的**意图**值（KV）。不要用 core.allowLan：那是内核运行时的
    // /configs 回读值，内核没跑时它一直是旧值/false，会让开关写完立刻被泵打回去。
    bool allowLan = false;
    bool serviceInstalled = false;    // root 服务单元是否存在

    bool operator==(const CoreView&) const = default;
};

struct CoreModel {
    // 唯一订阅点。必须带初值：huxerui::State 的默认构造是「空 cell」，
    // 读取（DriveCoreModel 每拍读一次）会抛 "Lifecycle dependency State is empty"。
    huxerui::State<CoreView> view{CoreView{}};
    // 外部来源（CLI/托盘/内核推送/OS）改动后自增：驱动循环跳过本次等待，
    // 立即重读一次。它**不会**唤醒正在 Delay 的泵（tick 只在读完后被检查），
    // 所以用户自己的动作要在完成回调里用 Update() 写透，不要靠它刷新界面。
    huxerui::State<std::uint64_t> refreshTick{0};

    void RequestRefresh() { refreshTick = refreshTick.Get() + 1; }

    // 写透：动作完成时（协程已经在 UI 线程）把权威值直接发布进模型，界面不必
    // 等下一个泵节拍，也不会出现"只改了控件本地 State、没人回写"的闪回。
    template <class Change> void Update(Change&& change) {
        CoreView next = view.Get();
        change(next);
        view = std::move(next);
    }
};

// 在 worker 线程读一次内核/接管状态（会做 checkAlive 崩溃检测；slowProbes
// 为 true 时才查询系统代理实际指向）。previousSystemProxyActive 用于在不做
// 昂贵探测时保持上一次的值——State 只能在 UI 线程读，所以由调用方按值传入。
CoreView ReadCoreView(bool slowProbes, bool previousSystemProxyActive);

// 启动唯一的状态泵（组合期调用一次）。
void DriveCoreModel(huxerui::TaskScope tasks,
                    std::shared_ptr<CoreModel> model);

} // namespace clashflux::ui
