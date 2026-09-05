// db.cppm — clashflux.db：SQLite 持久化（接口模块）。
//
// 两类数据：
//   profiles  —— 订阅（远程 URL 或本地文件；selected 标记当前启用）
//   settings  —— 应用设置 KV（主题模式、控制器 secret、系统代理开关等）
// SQLiteCpp 头只进实现单元（db.cpp 全局模块片段），接口只暴露纯数据结构。
export module clashflux.db;

import std;

namespace db {

export struct Profile {
    std::int64_t id = 0;         // 0 = 未保存过
    std::string name;
    std::string url;             // 订阅 URL；本地导入为空
    std::string file;            // profiles/ 下的 YAML 文件名（<id>.yaml）
    bool selected = false;       // 当前启用
    std::int64_t updatedAt = 0;  // 上次成功拉取/导入时间（Unix 秒）
    std::string error;           // 上次拉取错误（成功清空）

    bool operator==(const Profile&) const = default;  // State 变更检测
};

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
