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
#include "profile_list_model.h"

namespace clashflux::ui {

struct ProfilesModel : ProfileListModel<db::Profile> {
    // Read only the hydrated in-memory cache; no disk/network work on the UI
    // thread. Action completion publishes before feedback/navigation.
    void PublishCached();
    huxerui::State<std::vector<profile_link::RemoteProfile>> importLinks{
        std::vector<profile_link::RemoteProfile>{}};
    huxerui::State<std::string> linkError{std::string{}};
};

// 启动唯一的数据泵（组合期调用一次）：1s 做一次「修订号脏检查」，变了才
// 拷贝列表；动作完成直接 PublishCached，syncTick 使在途旧快照失效。
void DriveProfilesModel(huxerui::TaskScope tasks,
                        std::shared_ptr<ProfilesModel> model);

} // namespace clashflux::ui
