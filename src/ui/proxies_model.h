// proxies_model.h — 策略组快照的唯一来源（跨页面共享的 application service）。
//
// 此前首页卡片、代理页、托盘菜单各自用 2s / 3s / 1s 的定时器调用
// ProxyGroupsSnapshot() 拉 /proxies 再各自解析：同一份数据每秒被请求约 1.8 次、
// 解析三遍；而 REST 失败时会静默回落到「按订阅编译的预览」，UI 完全无法区分
// 实时数据与预览，用户会以为切换已经生效。
//
// 现在只有一个拉取器（common.cpp 的 FetchProxiesSnapshot）与一个驱动循环
// （DriveProxiesModel，在 AppRoot 组合期启动一次，2s 一拍、有请求时立即补一拍），
// 结果写进这里的 State。三个消费者读同一份 State 即可：State::Write 按
// operator== 去重，内容没变不会通知订阅者，所以「多一个消费者」不再意味着多一次
// 网络请求或多一次解析。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui.h" // ProxyGroupSnapshot

namespace clashflux::ui {

// 快照来源。Live = 内核/服务实时数据；Preview = 内核未运行时按当前订阅编译出的
// 预览（可预先选节点）；Empty = 无订阅或编译失败。UI 必须能区分这两者。
enum class ProxiesSource {
    Empty,
    Live,
    Preview,
};

struct ProxiesSnapshot {
    std::vector<ProxyGroupSnapshot> groups; // 扁平表示（首页卡片 / 托盘菜单）
    std::string body;                       // 原文（代理页解析嵌套分组用）
    ProxiesSource source = ProxiesSource::Empty;
    // 回落成 Preview 时的原因（REST 失败文本）；Live 时为空。把静默降级变成
    // 可显示的信息。
    std::string error;

    bool operator==(const ProxiesSnapshot&) const = default;
};

// 应用级单例：在 application_hooks 里 Provide，组件用 UseService<ProxiesModel>()。
struct ProxiesModel {
    // 唯一的订阅点。读它即订阅：内容变化时读取方所在的部分重组。
    // 必须带初值：huxerui::State 的默认构造是「空 cell」，读取/写入都会抛
    // "Lifecycle dependency State is empty"。
    huxerui::State<ProxiesSnapshot> snapshot{ProxiesSnapshot{}};
    // 用户动作（切节点 / 测速 / 切换订阅）后自增：驱动循环看到计数变化就跳过
    // 本次等待、立即再拉一拍。动作方不需要知道数据怎么取。
    huxerui::State<std::uint64_t> refreshTick{0};

    void RequestRefresh() { refreshTick = refreshTick.Get() + 1; }
};

// 安装应用级 UI 模型（application_hooks 调用一次；定义在 common.cpp）。
// 与 app_http_client.h 的 InstallAppHttpClient 同一套写法。
void InstallClashFluxUiModels(huxerui::ApplicationContext& context);

// 在 worker 线程上取一次快照（纯阻塞 IO + 解析，不触碰 State）。
ProxiesSnapshot FetchProxiesSnapshot();

// 启动唯一的数据泵（组合期调用一次）。写入恒在 UI 线程，内容相等时去重。
void DriveProxiesModel(huxerui::TaskScope tasks,
                       std::shared_ptr<ProxiesModel> model);

} // namespace clashflux::ui
