// db.cppm — clashflux.db：持久化访问（接口模块）。
//
// 接口保持不变（profiles / settings），实现已由 huxerui::sqlite ORM
// （clashflux.persistence）承担：读走内存缓存，写更新缓存并标脏，
// 落库由启动流程的异步 flush 完成。SQLiteCpp 已不再使用。
export module clashflux.db;

import std;
import clashflux.model;

namespace db {

// Profile 的唯一定义在 clashflux.model（打破 db ↔ persistence 的模块环）；
// 这里保留 db::Profile 这个既有名字，调用点无需改名。
export using Profile = model::Profile;

// 数据库句柄。构造即打开 + 建表（幂等）。所有方法抛 std::runtime_error；
// 调用方（领域 store）负责兜底转状态消息。
export class Db {
public:
    explicit Db(const std::filesystem::path& file);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // ---- profiles ----
    std::vector<Profile> listProfiles();                       // id 升序（创建顺序）
    std::int64_t saveProfile(const Profile& p);                // id==0 插入，否则更新；返回 id
    void deleteProfile(std::int64_t id);
    // 把 id 设为唯一启用（其余置 0）；id==0 表示全部取消启用。
    void setSelectedProfile(std::int64_t id);

    // ---- settings（KV）----
    std::string getSetting(const std::string& key, const std::string& fallback = "");
    void setSetting(const std::string& key, const std::string& value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace db
