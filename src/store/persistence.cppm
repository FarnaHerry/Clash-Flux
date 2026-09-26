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

    /// 打开（含 0→1 迁移）数据库并 hydrate 设置缓存。应用线程调用一次。
    /// @return true 表示库已就绪；失败时 lastError 有诊断文本。
    huxerui::Task<bool> open(const std::filesystem::path& file);

    /// 落库所有标脏设置后关闭连接。
    huxerui::Task<void> close();

    [[nodiscard]] bool ready() const noexcept;

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
    /// 保存：id==0 时由应用分配新 id；同步更新缓存并标脏。返回保存后的 id。
    std::int64_t saveProfile(model::Profile profile);
    /// 从缓存删除并标脏（落库由 flushProfiles 完成）。
    bool deleteProfile(std::int64_t id);
    /// 把 id 设为唯一启用（id==0 表示全部取消），同步改缓存并标脏。
    bool setSelectedProfile(std::int64_t id);
    [[nodiscard]] bool hasPendingProfiles() const noexcept;
    /// 把标脏的订阅 upsert、已删除的订阅删除，一次落库。
    huxerui::Task<bool> flushProfiles();

    [[nodiscard]] std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 进程级单例：启动流程负责 open，其余代码只读缓存/写缓存。
export Persistence& persistence();

} // namespace clashflux::persistence
