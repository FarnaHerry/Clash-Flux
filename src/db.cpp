// db.cpp — clashflux.db 实现单元（SQLiteCpp）。
module;

#include <ctime>  // std::time
#include <SQLiteCpp/SQLiteCpp.h>

module clashflux.db;

import std;

namespace db {
namespace {

Profile rowToProfile(SQLite::Statement& q) {
    Profile p;
    p.id = q.getColumn(0).getInt64();
    p.name = q.getColumn(1).getString();
    p.url = q.getColumn(2).getString();
    p.file = q.getColumn(3).getString();
    p.selected = q.getColumn(4).getInt() != 0;
    p.updatedAt = q.getColumn(5).getInt64();
    p.error = q.getColumn(6).getString();
    p.type = q.getColumn(7).getString();
    p.description = q.getColumn(8).getString();
    p.timeoutSecs = q.getColumn(9).getInt();
    p.intervalMins = q.getColumn(10).getInt();
    p.autoUpdate = q.getColumn(11).getInt() != 0;
    p.useSystemProxy = q.getColumn(12).getInt() != 0;
    p.useCoreProxy = q.getColumn(13).getInt() != 0;
    p.allowInvalidCert = q.getColumn(14).getInt() != 0;
    return p;
}

constexpr const char* kProfileColumns =
    "id, name, url, file, selected, updated_at, error, type, description,"
    " timeout_secs, interval_mins, auto_update, use_system_proxy,"
    " use_core_proxy, allow_invalid_cert";

} // namespace

struct Db::Impl {
    SQLite::Database db;

    explicit Impl(const std::filesystem::path& file)
        : db(file.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
        db.exec("PRAGMA journal_mode=WAL;");
        db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS profiles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    url TEXT NOT NULL DEFAULT '',
    file TEXT NOT NULL DEFAULT '',
    selected INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    error TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT ''
);
)SQL");
        // 迁移：订阅选项列（订阅弹窗扩展）。老库补列，新库已含这些列时
        // ALTER 报 duplicate column，吞掉即可。
        for (const char* stmt : {
                 "ALTER TABLE profiles ADD COLUMN type TEXT NOT NULL DEFAULT 'remote'",
                 "ALTER TABLE profiles ADD COLUMN description TEXT NOT NULL DEFAULT ''",
                 "ALTER TABLE profiles ADD COLUMN timeout_secs INTEGER NOT NULL DEFAULT 60",
                 "ALTER TABLE profiles ADD COLUMN interval_mins INTEGER NOT NULL DEFAULT 0",
                 "ALTER TABLE profiles ADD COLUMN auto_update INTEGER NOT NULL DEFAULT 0",
                 "ALTER TABLE profiles ADD COLUMN use_system_proxy INTEGER NOT NULL DEFAULT 0",
                 "ALTER TABLE profiles ADD COLUMN use_core_proxy INTEGER NOT NULL DEFAULT 0",
                 "ALTER TABLE profiles ADD COLUMN allow_invalid_cert INTEGER NOT NULL DEFAULT 0"}) {
            try {
                db.exec(stmt);
            } catch (const SQLite::Exception&) {
            }
        }
    }
};

Db::Db(const std::filesystem::path& file) : impl_(std::make_unique<Impl>(file)) {}
Db::~Db() = default;

std::vector<Profile> Db::listProfiles() {
    std::vector<Profile> out;
    SQLite::Statement q(impl_->db,
        std::string("SELECT ") + kProfileColumns + " FROM profiles ORDER BY id ASC");
    while (q.executeStep()) out.push_back(rowToProfile(q));
    return out;
}

std::int64_t Db::saveProfile(const Profile& p) {
    if (p.id == 0) {
        SQLite::Statement q(impl_->db,
            "INSERT INTO profiles (name, url, file, selected, updated_at, error,"
            " type, description, timeout_secs, interval_mins, auto_update,"
            " use_system_proxy, use_core_proxy, allow_invalid_cert)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        q.bind(1, p.name);
        q.bind(2, p.url);
        q.bind(3, p.file);
        q.bind(4, p.selected ? 1 : 0);
        q.bind(5, p.updatedAt);
        q.bind(6, p.error);
        q.bind(7, p.type);
        q.bind(8, p.description);
        q.bind(9, p.timeoutSecs);
        q.bind(10, p.intervalMins);
        q.bind(11, p.autoUpdate ? 1 : 0);
        q.bind(12, p.useSystemProxy ? 1 : 0);
        q.bind(13, p.useCoreProxy ? 1 : 0);
        q.bind(14, p.allowInvalidCert ? 1 : 0);
        q.exec();
        return impl_->db.getLastInsertRowid();
    }
    SQLite::Statement q(impl_->db,
        "UPDATE profiles SET name=?, url=?, file=?, selected=?, updated_at=?, error=?,"
        " type=?, description=?, timeout_secs=?, interval_mins=?, auto_update=?,"
        " use_system_proxy=?, use_core_proxy=?, allow_invalid_cert=? WHERE id=?");
    q.bind(1, p.name);
    q.bind(2, p.url);
    q.bind(3, p.file);
    q.bind(4, p.selected ? 1 : 0);
    q.bind(5, p.updatedAt);
    q.bind(6, p.error);
    q.bind(7, p.type);
    q.bind(8, p.description);
    q.bind(9, p.timeoutSecs);
    q.bind(10, p.intervalMins);
    q.bind(11, p.autoUpdate ? 1 : 0);
    q.bind(12, p.useSystemProxy ? 1 : 0);
    q.bind(13, p.useCoreProxy ? 1 : 0);
    q.bind(14, p.allowInvalidCert ? 1 : 0);
    q.bind(15, p.id);
    q.exec();
    return p.id;
}

void Db::deleteProfile(std::int64_t id) {
    SQLite::Statement q(impl_->db, "DELETE FROM profiles WHERE id=?");
    q.bind(1, id);
    q.exec();
}

void Db::setSelectedProfile(std::int64_t id) {
    SQLite::Transaction tx(impl_->db);
    impl_->db.exec("UPDATE profiles SET selected=0");
    if (id != 0) {
        SQLite::Statement q(impl_->db, "UPDATE profiles SET selected=1 WHERE id=?");
        q.bind(1, id);
        q.exec();
    }
    tx.commit();
}

std::string Db::getSetting(const std::string& key, const std::string& fallback) {
    SQLite::Statement q(impl_->db, "SELECT value FROM settings WHERE key=?");
    q.bind(1, key);
    if (q.executeStep()) return q.getColumn(0).getString();
    return fallback;
}

void Db::setSetting(const std::string& key, const std::string& value) {
    SQLite::Statement q(impl_->db,
        "INSERT INTO settings (key, value) VALUES (?, ?)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.bind(1, key);
    q.bind(2, value);
    q.exec();
}

} // namespace db
