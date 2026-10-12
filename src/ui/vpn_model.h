// vpn_model.h — 原生连接（PPTP / OpenVPN）状态的唯一来源。
//
// 此前订阅页自己挂一个 0.5s 循环，把 vpnStore().states()/openVpnStates() 拷进
// 两个 StateList；状态本身由 VpnStore 的 1s 监视线程维护。现在把能力与状态收进
// 应用级模型，页面只读模型（以模型 State 为依赖做镜像），不再自建定时器。
//
// 与订阅不同，这里的快照只有"用户配置过的原生连接"那么多条、每条几个短字符串，
// 所以驱动器用「取回后逐字段比较」判脏即可（不做存储层修订号），代价可以忽略。
//
// 注意：本头用到 store::VpnSnapshot，必须在
// `import clashflux.store.vpn;` 之后包含。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <vector>

#include "ui.h"
#include "vpn_snapshot_model.h"

namespace clashflux::ui {

// 用户动作在任务线程取得权威快照，恢复 UI 线程后 Publish 立即写透。
// RequestSync 仅请求下一拍，不能唤醒 Delay；旧轮询由 PublishPolled 拒绝。
struct VpnModel : VpnSnapshotModel<store::VpnSnapshot> {};

// 启动唯一的数据泵（组合期调用一次）：1s 一拍取回能力/连接快照，变化才发布。
void DriveVpnModel(huxerui::TaskScope tasks, std::shared_ptr<VpnModel> model);

} // namespace clashflux::ui
