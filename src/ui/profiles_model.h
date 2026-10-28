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
#include <map>
#include <string>
#include <vector>

#include "../profile_link.h"
#include "../profile_rules.h"

#include "ui.h"
#include "profile_list_model.h"

namespace clashflux::ui {

struct ProfilesModel : ProfileListModel<db::Profile> {
    struct RefreshFailureNotice {
        std::int64_t profileId = 0;
        std::string profileName;
        std::string error;
        bool operator==(const RefreshFailureNotice&) const = default;
    };

    // Read only the hydrated in-memory cache; no disk/network work on the UI
    // thread. Action completion publishes before feedback/navigation.
    void PublishCached();
    void ReportProfileRefreshFailure(std::int64_t id, std::string name,
                                     std::string error,
                                     bool userInitiated = false);
    void ClearProfileRefreshFailure(std::int64_t id);
    std::vector<RefreshFailureNotice> TakeProfileRefreshFailures();

    huxerui::State<std::vector<RefreshFailureNotice>> profileRefreshFailures{
        std::vector<RefreshFailureNotice>{}};
    std::map<std::int64_t, bool> reportedProfileErrors;
    huxerui::State<std::vector<profile_link::RemoteProfile>> importLinks{
        std::vector<profile_link::RemoteProfile>{}};
    huxerui::State<std::string> linkError{std::string{}};
    huxerui::State<std::int64_t> rulesProfile{0};
    huxerui::State<std::uint64_t> rulesRequests{0};
    huxerui::State<std::size_t> rulesView{0};
    huxerui::State<std::shared_ptr<const wire::RuleSetLibrary>> ruleSetLibrary{
        std::make_shared<const wire::RuleSetLibrary>()};
    huxerui::State<bool> ruleSetsLoaded{false};
    huxerui::State<bool> ruleSetsSaving{false};
    huxerui::State<std::uint64_t> ruleSetsRevision{0};
    // Receipts of explicit user actions in this session, not native auto-update telemetry.
    huxerui::State<std::map<std::string, std::string>> ruleSetRefreshReceipts{std::map<std::string, std::string>{}};
    std::map<std::pair<std::int64_t, std::size_t>, huxerui::ScrollController> rulesScrolls;
};

// 启动唯一的数据泵（组合期调用一次）：1s 做一次「修订号脏检查」，变了才
// 拷贝列表；动作完成直接 PublishCached，syncTick 使在途旧快照失效。
void DriveProfilesModel(huxerui::TaskScope tasks,
                        std::shared_ptr<ProfilesModel> model);

} // namespace clashflux::ui
