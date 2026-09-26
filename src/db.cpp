// db.cpp — clashflux.db 实现单元。
//
// SQLiteCpp 已被 huxerui::sqlite ORM（clashflux.persistence）取代：Db 现在
// 只是同步门面——读走 Persistence 的内存缓存，写更新缓存并标脏，真正落库由应用
// 启动流程里的异步 flush 泵完成。公开接口保持不变，所有调用点无需改动。
module;

module clashflux.db;

import std;
import clashflux.persistence;

namespace db {

struct Db::Impl {
    std::filesystem::path file;  // 仅诊断用；数据库由 persistence() 打开
};

Db::Db(const std::filesystem::path& file)
    : impl_(std::make_unique<Impl>(Impl{.file = file})) {}
Db::~Db() = default;

std::vector<Profile> Db::listProfiles() {
    return clashflux::persistence::persistence().listProfiles();
}

std::int64_t Db::saveProfile(const Profile& profile) {
    return clashflux::persistence::persistence().saveProfile(profile);
}

void Db::deleteProfile(std::int64_t id) {
    static_cast<void>(clashflux::persistence::persistence().deleteProfile(id));
}

void Db::setSelectedProfile(std::int64_t id) {
    static_cast<void>(clashflux::persistence::persistence().setSelectedProfile(id));
}

std::string Db::getSetting(const std::string& key,
                           const std::string& fallback) {
    return clashflux::persistence::persistence().setting(key, fallback);
}

void Db::setSetting(const std::string& key, const std::string& value) {
    clashflux::persistence::persistence().setSetting(key, value);
}

} // namespace db
