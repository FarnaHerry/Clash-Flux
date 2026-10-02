// profiles_model.h — 订阅列表的唯一来源（跨页面共享的 application service）。
//
// 此前订阅数据有三份来源：AppRoot 的 2s 全量泵（每拍从 persistence 拷一遍
// StateList）、托盘的 1s `profilesStore().list()`、以及各页面自己读存储层。
// 3 份数据不仅重复拷贝，还会互相打架（乐观选中被另一份覆盖）。
//
// 现在只有这一个模型：AppRoot 驱动唯一的数据泵，页面/托盘/rules 页只读它的
// State。脏检查用 persistence 的单调修订号（见 profilesRevision）：每拍只比较
// 一个整数，只有真的变了才拷贝整张表——订阅可能带 nativeConfig/nativeRoutes
// 大字段，避免"每个节拍都整表拷贝 + 逐字段比较"。
//
// 注意：本头文件用到 db::Profile（clashflux.model），必须在
// `import clashflux.db;` 之后包含（与 profiles_cache.h 同一约定）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <vector>

#include "../profile_link.h"

#include "ui.h"

namespace clashflux::ui {

struct ProfilesModel {
    // 权威列表：内容变化才写（State::Write 按 operator== 去重）。
    // 必须带初值：huxerui::State 的默认构造是「空 cell」，Get/写入/作为
    // Lifecycle 依赖都会抛 "Lifecycle dependency State is empty"。
    huxerui::State<std::vector<db::Profile>> list{std::vector<db::Profile>{}};
    // 乐观选中标记：为 true 时泵暂停发布，避免未确认的选中态被闪回。
    // 页面与模型共享同一个 State。
    huxerui::State<bool> selectionPending{false};
    // 显式请求立即同步（CRUD/激活/CLI 转发之后）。
    huxerui::State<std::uint64_t> syncTick{0};
    huxerui::State<std::vector<profile_link::RemoteProfile>> importLinks{
        std::vector<profile_link::RemoteProfile>{}};
    huxerui::State<std::string> linkError{std::string{}};

    void RequestSync() { syncTick = syncTick.Get() + 1; }
};

// 启动唯一的数据泵（组合期调用一次）：1s 做一次「修订号脏检查」，变了才
// 拷贝列表；syncTick 变化时立刻同步一次。
void DriveProfilesModel(huxerui::TaskScope tasks,
                        std::shared_ptr<ProfilesModel> model);

} // namespace clashflux::ui
