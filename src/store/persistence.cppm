// persistence.cppm — ORM 持久化服务：SQLite(ORM) 句柄 + settings 内存缓存。
//
// 读路径（setting）永远走内存缓存，组合期调用不碰磁盘、不阻塞 UI；
// 写路径（setSetting）同步更新缓存并标脏，flushSettings 在应用任务里异步落库。
// 数据库打开与 hydrate 用 huxerui::Task 异步完成，调用点在应用 TaskScope 上。
module;
#include <huxerui/sqlite.h>
#include "sqlite_schema.h"

export module clashflux.persistence;

import std;
import clashflux.model;

namespace clashflux::persistence {

// 首帧唯一的同步磁盘读：临时只读连接，不创建/迁移数据库，不填充订阅缓存。
export struct StartupTheme {
    int mode = 1;
    std::string error;
};
export StartupTheme readStartupTheme(const std::filesystem::path& file);

/// 进程内唯一的 ORM 数据库句柄与设置缓存。
///
/// 生命周期由启动流程（ApplicationHook/应用任务）管理：open 一次、close 一次；
/// 期间任意线程可同步读缓存，写缓存后由 flushSettings 落库。
///
/// 日志不在这里：日志是高频追加，直接写 core/*.log 文件，避免与
/// settings/profiles 的写事务抢锁与 IO。
export class Persistence {
public:
    Persistence();
    ~Persistence();
    Persistence(const Persistence&) = delete;
    Persistence& operator=(const Persistence&) = delete;

    /// 打开数据库并 hydrate settings/profiles 缓存。应用线程调用一次。
    /// 老库（user_version=0）由 0→1 迁移重建表并保留全部数据；迁移不了的库
    /// open 返回 false，应用不删除任何文件，lastError 有诊断文本。
    /// @return true 表示库已就绪；失败时 lastError 有诊断文本。
    huxerui::Task<bool> open(const std::filesystem::path& file,
                           const std::filesystem::path& profileDirectory = {});

    /// 落库所有标脏设置后关闭连接。
    huxerui::Task<void> close();

    [[nodiscard]] bool ready() const noexcept;

    /// 打开/hydrate 失败；不是允许空缓存继续运行的模式。订阅写入口拒绝操作，
    /// 启动方必须报告失败并停止正常初始化，保留原库与文件。
    [[nodiscard]] bool degraded() const noexcept;

    /// 同步读缓存；未 hydrate 或键不存在返回 fallback。
    [[nodiscard]] std::string setting(const std::string& key,
                                      const std::string& fallback = {}) const;

    /// 同步写缓存并标脏；不触碰磁盘。落库由 flushSettings 完成。
    void setSetting(const std::string& key, const std::string& value);

    [[nodiscard]] bool hasPendingSettings() const noexcept;

    /// 把标脏设置 upsert 进库；只清除本次成功写入的键，避免与并发写竞态。
    huxerui::Task<bool> flushSettings();

    // ---- profiles（内存缓存 + 异步落库；与 settings 同样的写后缓存模型）----
    /// 列出订阅（id 升序），读内存缓存。
    [[nodiscard]] std::vector<model::Profile> listProfiles() const;
    /// 保存：id==0 时由应用分配新 id；未 hydrate/打开失败/关闭中抛异常。
    std::int64_t saveProfile(model::Profile profile);
    /// 从缓存删除并标脏（落库由 flushProfiles 完成）。
    /// removeFile 仅供用户明确删除；数据库删除提交且无磁盘/缓存引用后才清理。
    /// 未提供 profileDirectory 时保留文件，绝不猜测路径或启动扫描清理。
    bool deleteProfile(std::int64_t id, bool removeFile = false);
    /// 把 id 设为唯一启用（id==0 表示全部取消），同步改缓存并标脏。
    bool setSelectedProfile(std::int64_t id);
    [[nodiscard]] bool hasPendingProfiles() const noexcept;
    /// 把标脏的订阅 upsert、已删除的订阅删除，一次落库。
    huxerui::Task<bool> flushProfiles();

    /// 订阅列表内容的单调修订号：**任何**一次 saveProfile / deleteProfile /
    /// setSelectedProfile 都会 +1（含 CLI、后台自动更新等所有路径），
    /// open() 完成 hydrate 也 +1（首帧读到空表的消费者因此还能收到通知）。
    /// UI 侧用它做「脏检查」：每拍只比较一个整数，而不是每次把整张表拷贝出来
    /// 再逐字段比较（订阅可能带 nativeConfig/nativeRoutes 大字段）。
    [[nodiscard]] std::uint64_t profilesRevision() const noexcept;

    [[nodiscard]] std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 进程级单例：启动流程负责 open，其余代码只读缓存/写缓存。
export Persistence& persistence();

} // namespace clashflux::persistence
